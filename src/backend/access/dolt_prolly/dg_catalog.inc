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

bool
dg_catalog_active(void)
{
	Relation	refs;
	SysScanDesc scan;
	ScanKeyData key;
	bool		active;
	bool		pushed = false;

	if (!IsUnderPostmaster || catalog_api_depth || rangevar_projection_hook)
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	refs = table_open(DoltRefRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum("views/enabled"));
	scan = systable_beginscan(refs, DoltRefKeyIndexId, true,
							  GetActiveSnapshot(), 1, &key);
	active = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(refs, AccessShareLock);
	if (pushed)
		PopActiveSnapshot();
	return active;
}

static TupleDesc
catalog_bind(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte, bool target)
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

	if (rangevar_projection_hook || catalog_api_depth)
		return NULL;			/* isolated phase-1 fixture */
	if (!rv->dolt_revision)
	{
		if (!rv->catalogname || !(sep = strpbrk(rv->catalogname, "@/")))
		{
			Oid			systemid;

			if (!dg_catalog_active())
				return NULL;
			if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
				return NULL;
			systemid = (!rv->schemaname || !strcmp(rv->schemaname, "pg_catalog")) ? get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE) : InvalidOid;
			if (OidIsValid(systemid))
			{
				RangeVar   *qualified;

				if (systemid >= FirstNormalObjectId || !strncmp(rv->relname, "pg_dolt_", 8))
					return NULL;
				/* Stored catalog views still contain context-free RTEs. */
				if (!strcmp(rv->relname, "pg_tables") || !strcmp(rv->relname, "pg_indexes") || !strcmp(rv->relname, "pg_views") || !strcmp(rv->relname, "pg_matviews") || !strcmp(rv->relname, "pg_sequences") || !strcmp(rv->relname, "pg_stats") || !strcmp(rv->relname, "pg_stats_ext") || !strcmp(rv->relname, "pg_stats_ext_exprs"))
					unsupported("stored catalog views over versioned objects");
				/* Only supported versioned catalog families are redirected. */
				if (!list_member_oid(list_make5_oid(NamespaceRelationId, RelationRelationId, AttributeRelationId, TypeRelationId, IndexRelationId), systemid) && systemid != ConstraintRelationId && systemid != DescriptionRelationId && systemid != DependRelationId && systemid != CastRelationId)
					return NULL;
				qualified = (RangeVar *) copyObject(rv);
				qualified->schemaname = pstrdup("pg_catalog");
				rv = qualified;
			}
			revision = pstrdup(dg_current_branch());
			goto revision_selected;
		}
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
revision_selected:
	if (target && (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/"))))
		unsupported("revision-qualified write targets");
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
					if (!target)
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
					if (!working && OidIsValid(currentns) && object_aclcheck(NamespaceRelationId, currentns, user, ACL_USAGE) != ACLCHECK_OK)
						aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, ns);
					if (!working && !target && OidIsValid(currentrel) && pg_class_aclcheck(currentrel, user, ACL_SELECT) != ACLCHECK_OK)
						aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);
					break;
				}
			}
			if (!descriptor)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg(working ? "relation %s does not exist on branch %s" : "relation %s does not exist in revision %s", rv->relname, revision)));
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

/* Internal executor/DDL integration boundary. No SQL preparation API. Resolve
 * authority from canonical membership before constructing a private AM relation. */
static Relation
catalog_open_table(const char *branch, const char *name, AclMode permission)
{
	Oid			user;
	int			security,
				nest;
	Relation	result = NULL;
	List	   *parts = stringToQualifiedNameList(name, NULL);
	const char *privilege;

	if (!ActiveSnapshotSet() || list_length(parts) != 2)
		elog(ERROR, "canonical write requires a snapshot and qualified logical name");
	switch (permission)
	{
		case ACL_SELECT:
			privilege = "SELECT";
			break;
		case ACL_INSERT:
			privilege = "INSERT";
			break;
		case ACL_UPDATE:
			privilege = "UPDATE";
			break;
		case ACL_DELETE:
			privilege = "DELETE";
			break;
		default:
			elog(ERROR, "invalid canonical write permission");
	}
	if (permission != ACL_SELECT)
		dg_storage_lock_working(branch, GetActiveSnapshot());
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db,
				   *descriptor,
				   *schema;
		char	   *definition;
		Oid			resource,
					handle;
		DGStorage  *storage;

		nest = begin_api();
		db = read_ref(psprintf("working/%s", branch)).target;
		if (!db)
			elog(ERROR, "canonical working set disappeared");
		run("SELECT t.objhash FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
		if (SPI_processed != 1)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", name, branch)));
		descriptor = DatumGetByteaPCopy(cell(0, 1));
		schema = child(descriptor, 1);
		definition = metadata(schema, "schema");
		catalog_acl(catalog_namespace(db, strVal(linitial(parts))), "USAGE", user);
		catalog_acl(definition, privilege, user);
		resource = binding_relation(branch, name, schema);
		if (!OidIsValid(resource))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", name, branch)));
		handle = CatalogViewLookupObject(provider_view(db), RelationRelationId, name);
		storage = dg_storage_open(handle, branch, name, schema, resource, GetActiveSnapshot());
		result = permission == ACL_SELECT ? dg_storage_enable_read(storage, child(descriptor, 2)) : dg_storage_enable_write(storage, child(descriptor, 2));
		if (permission != ACL_SELECT)
		{
			List	   *ids = RelationGetIndexList(result);
			List	   *indexes = NIL;
			ListCell   *lc;

			foreach(lc, ids)
			{
				Oid			indexid = lfirst_oid(lc);
				char	   *indexname = quote_qualified_identifier(get_namespace_name(get_rel_namespace(indexid)), get_rel_name(indexid));
				bytea	   *indexdesc,
						   *indexschema;
				Oid			indexresource;
				DGStorage  *indexstorage;

				run("SELECT t.objhash FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='index' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(indexname)), NULL, true);
				if (SPI_processed != 1)
					elog(ERROR, "canonical index membership is unavailable");
				indexdesc = DatumGetByteaPCopy(cell(0, 1));
				indexschema = child(indexdesc, 1);
				indexresource = binding_relation(branch, indexname, indexschema);
				if (!OidIsValid(indexresource))
					elog(ERROR, "canonical index storage binding is unavailable");
				indexstorage = dg_storage_open(indexid, branch, indexname, indexschema, indexresource, GetActiveSnapshot());
				indexes = lappend(indexes, dg_storage_enable_write(indexstorage, child(indexdesc, 2)));
			}
			dg_storage_set_indexes(result, indexes);
			list_free(ids);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return result;
}

TupleDesc
dg_catalog_bind(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte)
{
	return catalog_bind(pstate, rv, rte, false);
}

Relation
dg_catalog_target(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte)
{
	TupleDesc	desc = catalog_bind(pstate, rv, rte, true);

	if (!desc)
		return NULL;
	if (!rte->dolt_branch || rte->dolt_catalog)
		unsupported("non-working canonical write target");
	return table_open(get_typ_typrelid(rte->dolt_rowtype), NoLock);
}

Relation
dg_catalog_executor_target(RangeTblEntry *rte, CmdType command)
{
	char	   *name = quote_qualified_identifier(get_namespace_name(get_rel_namespace(rte->relid)), get_rel_name(rte->relid));
	AclMode		permission = command == CMD_INSERT ? ACL_INSERT : command == CMD_UPDATE ? ACL_UPDATE : ACL_DELETE;
	Relation	rel;

	if (command != CMD_INSERT && command != CMD_UPDATE && command != CMD_DELETE)
		unsupported("canonical executor operation");
	rel = dg_catalog_write_table(rte->dolt_branch, name, permission);
	if (RelationGetRelid(rel) != rte->relid)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("canonical target changed; replan statement")));
	return rel;
}

/* Name-to-OID conversions (regclass and native utility lookup) obey the same
 * selected branch as relation references. The result is a definition handle;
 * resource authority is obtained separately by supported executor paths. */
bool
dg_catalog_lookup_relation(const RangeVar *rv, bool missing_ok, Oid *result)
{
	Oid			user;
	int			security,
				nest;
	List	   *path = NIL;
	ListCell   *lc;
	bool		pushed = false;
	char	   *search_path;

	if (!dg_catalog_active())
		return false;
	if (rv->schemaname && !strcmp(rv->schemaname, "pg_catalog"))
		return false;
	if (!rv->schemaname && OidIsValid(get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE)))
		return false;
	if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
		return false;
	if (rv->dolt_revision)
		unsupported("revision-qualified native utility lookup");
	search_path = pstrdup(GetConfigOption("search_path", false, false));
	GetUserIdAndSecContext(&user, &security);
	if (rv->schemaname)
		path = list_make1(rv->schemaname);
	else if (!SplitIdentifierString(search_path, ',', &path))
		elog(ERROR, "invalid search_path");
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	*result = InvalidOid;
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db;

		nest = begin_api();
		db = read_ref(branch_ref("working")).target;
		foreach(lc, path)
		{
			char	   *ns = (char *) lfirst(lc),
					   *name;

			if (!strcmp(ns, "$user"))
				ns = GetUserNameFromId(user, false);
			name = quote_qualified_identifier(ns, rv->relname);
			if (catalog_contains(db, name))
			{
				catalog_acl(catalog_namespace(db, ns), "USAGE", user);
				*result = CatalogViewLookupObject(provider_view(db), RelationRelationId, name);
				break;
			}
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	if (!OidIsValid(*result) && !missing_ok)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", rv->relname, dg_current_branch())));
	return true;
}

Relation
dg_catalog_write_table(const char *branch, const char *name, AclMode permission)
{
	if (permission != ACL_INSERT && permission != ACL_UPDATE && permission != ACL_DELETE)
		elog(ERROR, "invalid canonical write permission");
	return catalog_open_table(branch, name, permission);
}

Relation
dg_catalog_copy_relation(Relation definition, bool from)
{
	char	   *name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(definition)), RelationGetRelationName(definition));
	Relation	rel = catalog_open_table(dg_current_branch(), name, from ? ACL_INSERT : ACL_SELECT);

	if (RelationGetRelid(rel) != RelationGetRelid(definition))
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("canonical COPY definition changed; retry statement")));
	return rel;
}

/* Caller has performed native LOCK TABLE ACL checks on the bound definition. */
bool
dg_catalog_lock_relation(Oid id, LOCKMODE mode, bool nowait)
{
	Relation	definition;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;

	if (!CatalogViewObjectContext(id))
		return false;
	definition = relation_open(id, NoLock);
	if (!definition->rd_catalogview)
	{
		relation_close(definition, NoLock);
		return false;
	}
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		char	   *name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(definition)), RelationGetRelationName(definition));
		bytea	   *db,
				   *descriptor,
				   *schema;
		Oid			resource;

		nest = begin_api();
		db = read_ref(branch_ref("working")).target;
		run("SELECT t.objhash FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
		if (SPI_processed != 1)
			elog(ERROR, "canonical lock target disappeared");
		descriptor = DatumGetByteaPCopy(cell(0, 1));
		schema = child(descriptor, 1);
		resource = binding_relation(dg_current_branch(), name, schema);
		if (!OidIsValid(resource))
			elog(ERROR, "canonical lock resource unavailable");
		if (!nowait)
			LockRelationOid(resource, mode);
		else if (!ConditionalLockRelationOid(resource, mode))
			ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE), errmsg("could not obtain lock on relation \"%s\"", RelationGetRelationName(definition))));
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	} PG_END_TRY();
	relation_close(definition, NoLock);
	return true;
}
