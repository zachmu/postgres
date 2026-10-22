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
				nest;
	bytea	   *db,
			   *descriptor = NULL;

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
		rte->dolt_database = hex(db);
		if (rv->schemaname && !strcmp(rv->schemaname, "pg_catalog"))
		{
			Relation	catalog;
			Oid			id = get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE);

			provider_catalog_index(id); /* reject unsupported catalogs
										 * explicitly */
			(void) provider_view(db);
			if (pg_class_aclcheck(id, user, ACL_SELECT) != ACLCHECK_OK)
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);
			catalog = table_open(id, AccessShareLock);
			result = CreateTupleDescCopy(RelationGetDescr(catalog));
			rte->dolt_catalog = id;
			rte->dolt_table = pstrdup("");
			rte->dolt_rowtype = get_rel_type_id(id);
			table_close(catalog, AccessShareLock);
		}
		else
		{
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
					 * Do not let historical grants bypass current revocations
					 * on a live object.
					 */
					currentns = get_namespace_oid(ns, true);
					currentrel = OidIsValid(currentns) ? get_relname_relid(rv->relname, currentns) : InvalidOid;
					if (working)
					{
						live = binding_relation(revision, logical, child(descriptor, 1));
						if (!OidIsValid(live))
							ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", logical, revision)));
					}
					if (!working)
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
			{
				CatalogView *view = provider_view(db);
				Oid			handle = CatalogViewLookupObject(view, RelationRelationId, logical);
				Relation	rel = relation_open(handle, AccessShareLock);

				result = CreateTupleDescCopy(RelationGetDescr(rel));
				rte->dolt_rowtype = rel->rd_rel->reltype;
				relation_close(rel, AccessShareLock);
			}

			if (working && OidIsValid(live))
			{
				rte->dolt_branch = pstrdup(revision);
				rte->dolt_relid = live;
			}

			if (working && !OidIsValid(live))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unpublished branch schema changes are not supported yet")));
		}
		end_api(nest);
	} PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	} PG_END_TRY();
	if (pushed)
		PopActiveSnapshot();
	return result;
}
