/* Versioned metadata lookup. SPI is restricted to fixed internal read queries;
 * permissions on the selected symbolic objects are checked for the caller. */
static void
catalog_acl(const char *definition, const char *privilege, Oid user)
{
	char	   *owner = json_value(definition, "owner");
	Oid			role;
	int			i,
				n;
	char	  **grantees;

	if (superuser_arg(user))
		return;
	role = owner ? get_role_oid(owner, true) : InvalidOid;
	if (OidIsValid(role) && has_privs_of_role(user, role))
		return;
	run("SELECT v->>1 FROM jsonb_array_elements($1::jsonb->'acl') v WHERE v->>2=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(privilege)), NULL, true);
	n = SPI_processed;
	grantees = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		grantees[i] = strcell(i, 1);
	for (i = 0; i < n; i++)
	{
		if (!strcmp(grantees[i], "PUBLIC"))
			return;
		role = get_role_oid(grantees[i], true);
		if (OidIsValid(role) && has_privs_of_role(user, role))
			return;
	}
	ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("permission denied for versioned object")));
}

void
dg_catalog_permission(const char *descriptor, Oid user)
{
	Oid			saved;
	int			security,
				nest;
	char	   *schema;

	GetUserIdAndSecContext(&saved, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		schema = metadata(child(unhex(descriptor), 1), "schema");
		catalog_acl(schema, "SELECT", user);
		end_api(nest);
	} PG_FINALLY();
	{
		SetUserIdAndSecContext(saved, security);
	} PG_END_TRY();
}

/* Namespace descriptions are decoded through the same canonical provider. */
static char *
catalog_namespace(bytea *db, const char *name)
{
	bytea	  **addresses;
	int			n,
				i;

	run("SELECT o.objhash FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object o ON o.objhash=ch WHERE d.objhash=$1 AND o.objkind='namespace'", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(db)), NULL, true);
	n = SPI_processed;
	addresses = palloc(sizeof(bytea *) * Max(n, 1));
	for (i = 0; i < n; i++)
		addresses[i] = DatumGetByteaPCopy(cell(i, 1));
	for (i = 0; i < n; i++)
	{
		char	   *definition = metadata(addresses[i], "namespace");
		char	   *candidate = json_value(definition, "name");

		if (candidate && !strcmp(candidate, name))
			return definition;
	}
	elog(ERROR, "missing retained namespace");
	return NULL;
}

TupleDesc
dg_catalog_bind(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte)
{
	const char *sep = NULL;
	char	   *revision,
			   *dbname,
			   *schema = NULL,
			   *logical = NULL;
	List	   *path = NIL;
	ListCell   *lc;
	Oid			user;
	int			security,
				nest,
				i,
				n;
	bytea	   *db,
			   *descriptor = NULL;
	char	  **names,
			  **types,
			  **collations;
	int		   *mods;
	TupleDesc	result = NULL;
	bool		pushed = false;
	bool		working = false;
	Oid			live = InvalidOid;

	if (rangevar_projection_hook)
		return NULL;			/* isolated phase-1 fixture */
	if (!rv->dolt_revision)
	{
		if (!rv->catalogname || !(sep = strpbrk(rv->catalogname, "@/")))
			return NULL;
		dbname = pnstrdup(rv->catalogname, sep - rv->catalogname);
		if (strcmp(dbname, get_database_name(MyDatabaseId)))
			return NULL;
		if (strlen(rv->catalogname) == NAMEDATALEN - 1)
			ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("revision-qualified identifier may have been truncated"), errhint("Use AS OF 'full hash' or a shorter qualifier.")));
		revision = pstrdup(sep + 1);
	}
	else
	{
		Node	   *expression = transformExpr(pstate, copyObject(rv->dolt_revision), EXPR_KIND_OTHER);

		expression = coerce_to_specific_type(pstate, expression, TEXTOID, "AS OF");
		assign_expr_collations(pstate, expression);
		expression = eval_const_expressions(NULL, expression);
		if (!IsA(expression, Const) || ((Const *) expression)->constisnull)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("AS OF requires a non-null constant revision expression")));
		revision = TextDatumGetCString(((Const *) expression)->constvalue);
		if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cross-database versioned queries are not supported")));
	}
	GetUserIdAndSecContext(&user, &security);
	if (rv->schemaname)
		path = list_make1(pstrdup(rv->schemaname));
	else
	{
		char	   *raw = pstrdup(GetConfigOption("search_path", false, false));

		if (!SplitIdentifierString(raw, ',', &path))
			elog(ERROR, "invalid search_path");
	}
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		working = !rv->dolt_revision && read_ref(psprintf("heads/%s", revision)).target != NULL;
		db = working ? read_ref(psprintf("working/%s", revision)).target : NULL;
		if (!db)
			db = child(resolve(revision), 1);
		foreach(lc, path)
		{
			char	   *ns = (char *) lfirst(lc);

			if (!strcmp(ns, "$user"))
				ns = GetUserNameFromId(user, false);
			logical = quote_qualified_identifier(ns, rv->relname);
			run("SELECT t.objhash FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(logical)), NULL, true);
			if (SPI_processed)
			{
				char	   *nsdef;
				Oid			currentns,
							currentrel;

				descriptor = DatumGetByteaPCopy(cell(0, 1));
				schema = metadata(child(descriptor, 1), "schema");
				nsdef = catalog_namespace(db, ns);
				catalog_acl(nsdef, "USAGE", user);
				catalog_acl(schema, "SELECT", user);

				/*
				 * Do not let historical grants bypass current revocations on
				 * a live object.
				 */
				currentns = get_namespace_oid(ns, true);
				currentrel = OidIsValid(currentns) ? get_relname_relid(rv->relname, currentns) : InvalidOid;
				if (working)
				{
					currentrel = binding_relation(revision, logical, child(descriptor, 1));
					if (!OidIsValid(currentrel))
						ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", logical, revision)));
				}
				live = currentrel;
				if (OidIsValid(currentns) && object_aclcheck(NamespaceRelationId, currentns, user, ACL_USAGE) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, ns);
				if (OidIsValid(currentrel) && pg_class_aclcheck(currentrel, user, ACL_SELECT) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);
				break;
			}
		}
		if (!descriptor)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist in revision %s", rv->relname, revision)));
		rte->dolt_table = hex(descriptor);
		run("SELECT v->>'name',v->>'type',(v->>'typmod')::int,CASE WHEN v->'collation' IS NOT NULL AND v->'collation'<>'null'::jsonb THEN quote_ident(v->'collation'->>0)||'.'||quote_ident(v->'collation'->>1) ELSE NULL END FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema)), NULL, true);
		n = SPI_processed;
		names = palloc(n * sizeof(char *));
		types = palloc(n * sizeof(char *));
		collations = palloc(n * sizeof(char *));
		mods = palloc(n * sizeof(int));
		for (i = 0; i < n; i++)
		{
			names[i] = strcell(i, 1);
			types[i] = strcell(i, 2);
			mods[i] = DatumGetInt32(cell(i, 3));
			collations[i] = strcell(i, 4);
		}
		result = CreateTemplateTupleDesc(n);
		for (i = 0; i < n; i++)
		{
			Oid			type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(types[i]), ObjectIdGetDatum(PG_CATALOG_NAMESPACE));

			if (!OidIsValid(type))
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("unknown retained built-in type %s", types[i])));
			TupleDescInitEntry(result, i + 1, names[i], type, mods[i], 0);
			if (collations[i])
				TupleDescInitEntryCollation(result, i + 1, get_collation_oid(stringToQualifiedNameList(collations[i], NULL), false));
		}
		dg_validate_descriptor(result);
		if (working && OidIsValid(live))
		{
			Relation	rel = table_open(live, AccessShareLock);
			TupleDesc	current = RelationGetDescr(rel);
			bool		compatible = current->natts == result->natts;

			for (i = 0; compatible && i < current->natts; i++)
			{
				Form_pg_attribute a = TupleDescAttr(current, i),
							b = TupleDescAttr(result, i);

				compatible = !a->attisdropped && a->atttypid == b->atttypid && a->atttypmod == b->atttypmod && a->attcollation == b->attcollation && !strcmp(NameStr(a->attname), NameStr(b->attname));
			}
			if (compatible && rel->rd_tableam == dg_table_routine())
			{
				rte->dolt_branch = pstrdup(revision);
				rte->dolt_relid = live;
			}
			else
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unpublished branch schema changes are not supported yet")));
			table_close(rel, NoLock);
		}
		if (working && !OidIsValid(live))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unpublished branch schema changes are not supported yet")));
		end_api(nest);
	} PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	} PG_END_TRY();
	if (pushed)
		PopActiveSnapshot();
	return result;
}
