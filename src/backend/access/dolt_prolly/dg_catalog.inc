/* Nested SQL inherits an immutable provider selected by its bound arguments. */
static const char *
catalog_current_database(void)
{
	DGProvider *entry;
	CatalogView *view = CatalogViewCurrent();

	if (!view || dg_execution_branch())
		return NULL;
	for (entry = providers; entry; entry = entry->next)
		if (entry->view == view)
			return entry->database;
	return NULL;
}

/* The branch head whose schema the current context view shows, if any. */
static const char *
catalog_current_branch(void)
{
	DGProvider *entry;
	CatalogView *view = CatalogViewCurrent();

	if (!view || dg_execution_branch())
		return NULL;
	for (entry = providers; entry; entry = entry->next)
		if (entry->view == view)
			return entry->branch;
	return NULL;
}

/* Names resolve on |branch|'s head when given, else in revision |db|. */
static CatalogView *
context_view(bytea *db, const char *branch)
{
	return branch ? provider_branch_view(branch, db) : provider_view(db);
}

/* All SQL entry paths use the provider's complete catalog family set. */
static bool
catalog_supported(Oid id)
{
	int			i;

	for (i = 0; i < lengthof(provider_catalogs); i++)
		if (provider_catalogs[i] == id)
			return true;
	return false;
}

/* Versioned metadata lookup. SPI is restricted to fixed internal read queries;
 * permissions on the selected symbolic objects are checked for the caller. */
static bool
catalog_acl_allows(const char *definition, const char *privilege, Oid user)
{
	char	   *owner = json_value(definition, "owner");
	Oid			role;
	int			i,
				n;
	char	  **grantees;

	if (superuser_arg(user))
		return true;
	role = owner ? get_role_oid(owner, true) : InvalidOid;
	if (OidIsValid(role) && has_privs_of_role(user, role))
		return true;
	run("SELECT v->>1 FROM jsonb_array_elements($1::jsonb->'acl') v WHERE v->>2=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(privilege)), NULL, true);
	n = SPI_processed;
	grantees = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		grantees[i] = strcell(i, 1);
	for (i = 0; i < n; i++)
	{
		if (!strcmp(grantees[i], "PUBLIC"))
			return true;
		role = get_role_oid(grantees[i], true);
		if (OidIsValid(role) && has_privs_of_role(user, role))
			return true;
	}
	return false;
}

static void
catalog_acl(const char *definition, const char *privilege, Oid user)
{
	if (!catalog_acl_allows(definition, privilege, user))
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
/* The definition of namespace |name| in schema |db|, or NULL. */
static char *
catalog_namespace_find(bytea *db, const char *name)
{
	bytea	  **addresses;
	int			n,
				i;

	run("SELECT o.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1 AND o.objkind='namespace'", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(db)), NULL, true);
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
	return NULL;
}

static char *
catalog_namespace(bytea *db, const char *name)
{
	char	   *definition = catalog_namespace_find(db, name);

	if (!definition)
		elog(ERROR, "missing retained namespace");
	return definition;
}

/* Resolve symbolic membership in SQL search-path order. Bootstrap catalogs are
 * candidates in that same order, never a shortcut ahead of an explicit path. */
static char *catalog_resolve_candidates(bytea *db, const RangeVar *rv, Oid user, Oid *native, const char *search_path, bool type_lookup, bool temp_ok);

static char *
catalog_resolve_name(bytea *db, const RangeVar *rv, Oid user, Oid *native, const char *search_path, bool type_lookup, bool temp_ok)
{
	CatalogView *previous = CatalogViewSwitch(NULL);
	char	   *result;

	/*
	 * Canonical candidates use the explicit database root. Shared system and
	 * temporary candidates use their physical namespace, even in nested
	 * scopes.
	 */
	PG_TRY();
	{
		result = catalog_resolve_candidates(db, rv, user, native, search_path, type_lookup, temp_ok);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

static char *
catalog_resolve_candidates(bytea *db, const RangeVar *rv, Oid user, Oid *native, const char *search_path, bool type_lookup, bool temp_ok)
{
	List	   *path = NIL;
	ListCell   *lc;
	bool		explicit = rv->schemaname != NULL;
	Oid			temp,
				toast;

	GetTempNamespaceState(&temp, &toast);

	*native = InvalidOid;
	if (explicit)
		path = list_make1(rv->schemaname);
	else
	{
		char	   *raw = pstrdup(search_path);
		bool		catalog = false;

		if (!SplitIdentifierString(raw, ',', &path))
			elog(ERROR, "invalid search_path");
		foreach(lc, path)
			if (!strcmp((char *) lfirst(lc), "pg_catalog"))
			catalog = true;
		if (!catalog)
			path = lcons("pg_catalog", path);
		if (temp_ok && OidIsValid(temp))
		{
			bool		listed = false;

			foreach(lc, path)
				if (!strcmp((char *) lfirst(lc), "pg_temp") ||
					!strcmp((char *) lfirst(lc), get_namespace_name(temp)))
				listed = true;
			if (!listed)
				path = lcons("pg_temp", path);
		}
	}
	foreach(lc, path)
	{
		char	   *ns = (char *) lfirst(lc);
		char	   *name;
		Oid			namespace;

		if (!strcmp(ns, "$user"))
			ns = GetUserNameFromId(user, false);
		if (dg_system_namespace(ns))
		{
			namespace = get_namespace_oid(ns, true);
			*native = OidIsValid(namespace) ? (type_lookup ? GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(rv->relname), ObjectIdGetDatum(namespace)) : get_relname_relid(rv->relname, namespace)) : InvalidOid;
			if (OidIsValid(*native))
				return ns;
			continue;
		}
		if (temp_ok && OidIsValid(temp) && (!strcmp(ns, "pg_temp") ||
											!strcmp(ns, get_namespace_name(temp))))
		{
			*native = type_lookup ? GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(rv->relname), ObjectIdGetDatum(temp)) : get_relname_relid(rv->relname, temp);
			if (OidIsValid(*native))
				return get_namespace_name(temp);
			continue;
		}
		name = quote_qualified_identifier(ns, rv->relname);
		if (type_lookup ? OidIsValid(CatalogViewFindObject(provider_schema_view(db), TypeRelationId, name)) : catalog_contains(db, name))
		{
			char	   *definition = catalog_namespace(db, ns);

			if (!catalog_acl_allows(definition, "USAGE", user))
			{
				if (explicit)
					catalog_acl(definition, "USAGE", user);
				continue;
			}
			return ns;
		}

		/* Unversioned native objects live beside the branch's objects. */
		namespace = get_namespace_oid(ns, true);
		if (OidIsValid(namespace))
		{
			Oid			physical = type_lookup ?
				GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(rv->relname), ObjectIdGetDatum(namespace)) :
				get_relname_relid(rv->relname, namespace);

			if (!OidIsValid(physical))
				continue;
			if (object_aclcheck(NamespaceRelationId, namespace, user, ACL_USAGE) != ACLCHECK_OK)
			{
				if (explicit)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, ns);
				continue;
			}
			*native = physical;
			return ns;
		}
	}
	return NULL;
}

/*
 * Whether the database is a repository: from its first prolly object onward,
 * every prolly reference resolves through a branch's canonical schema.
 */
bool
dg_catalog_active(void)
{
	Relation	refs;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	bool		active = false;

	if (!IsUnderPostmaster || catalog_api_depth || rangevar_projection_hook)
		return false;
	refs = table_open(DoltRefRelationId, AccessShareLock);
	ScanKeyEntryInitialize(&key, 0, Anum_pg_dolt_ref_refname, BTGreaterEqualStrategyNumber, InvalidOid,
						   C_COLLATION_OID, F_TEXT_GE, CStringGetTextDatum("heads/"));
	scan = systable_beginscan(refs, DoltRefKeyIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		bool		isnull;
		char	   *name = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_ref_refname, RelationGetDescr(refs), &isnull));

		active = strncmp(name, "heads/", 6) == 0;
	}
	systable_endscan(scan);
	table_close(refs, AccessShareLock);
	return active;
}

/* schema.dolt_<table> names the built-in alias unless the schema defines it. */
static const RangeVar *
system_alias(const RangeVar *rv)
{
	RangeVar   *alias;

	if (rv->schemaname && !strcmp(rv->schemaname, "public") &&
		(!strcmp(rv->relname, "dolt_docs") || !strcmp(rv->relname, "dolt_tests") || !strcmp(rv->relname, "dolt_query_catalog")))
		return rv;
	if (!rv->schemaname || !strcmp(rv->schemaname, "pg_catalog") || !OidIsValid(dg_system_relation(rv)))
		return rv;
	if (OidIsValid(get_namespace_oid(rv->schemaname, true)) &&
		OidIsValid(get_relname_relid(rv->relname, get_namespace_oid(rv->schemaname, true))))
		return rv;
	alias = copyObject(rv);
	alias->schemaname = "pg_catalog";
	return alias;
}

static TupleDesc
catalog_bind(ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte, bool target)
{
	const char *sep = NULL;
	char	   *resolution_path = pstrdup(GetConfigOption("search_path", false, false));
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
	const char *context_branch = NULL;

	if (rangevar_projection_hook || catalog_api_depth)
		return NULL;
	rv = system_alias(rv);
	if (!rte->dolt_database && !rv->dolt_revision &&
		!(rv->catalogname && strpbrk(rv->catalogname, "@/")) && catalog_current_database())
	{
		rte->dolt_database = pstrdup(catalog_current_database());
		context_branch = catalog_current_branch();
	}
	if (!rv->dolt_revision)
	{
		if (!rv->catalogname || !(sep = strpbrk(rv->catalogname, "@/")))
		{
			if (!rte->dolt_database && !dg_catalog_active())
				return NULL;
			if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
				return NULL;
			revision = pstrdup(context_branch ? context_branch : dg_current_branch());
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

	/* Names resolve against the latest schemas, as against catalogs. */
	if (!rte->dolt_database)
	{
		PushActiveSnapshot(GetLatestSnapshot());
		pushed = true;
	}
	else if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		working = (!rte->dolt_database || context_branch) && !rv->dolt_revision &&
			read_ref(psprintf("heads/%s", revision)).target != NULL;
		db = rte->dolt_database ? unhex(rte->dolt_database) :
			(working ? catalog_working_schema(revision) : NULL);
		if (!db)
			db = child(resolve(revision), 1);
		rte->dolt_database = hex(db);
		{
			Oid			native;
			char	   *ns = catalog_resolve_name(db, rv, user, &native, resolution_path, false, true);
			RangeVar   *resolved = copyObject(rv);

			if (!ns)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg(working ? "relation %s does not exist on branch %s" : "relation %s does not exist in revision %s", rv->relname, revision)));
			if (OidIsValid(native) && isTempNamespace(get_rel_namespace(native)))
			{
				if (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/")))
					unsupported("revision-qualified temporary relations");
				goto binding_done;
			}

			/* An unversioned native relation exists only on the live database. */
			if (OidIsValid(native) && !dg_system_namespace(ns))
			{
				if (!working)
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist in revision %s", rv->relname, revision)));
				goto binding_done;
			}
			resolved->schemaname = pstrdup(ns);
			rv = resolved;
			path = list_make1(resolved->schemaname);
		}

		if (rv->schemaname && dg_system_namespace(rv->schemaname))
		{
			Relation	catalog;
			Oid			id = get_relname_relid(rv->relname, get_namespace_oid(rv->schemaname, false));

			/* Built-in system tables are written natively (e.g. dolt_docs). */
			if (target)
				goto binding_done;
			if (!catalog_supported(id) && get_rel_relkind(id) != RELKIND_VIEW)
			{
				if (rv->dolt_revision || (rv->catalogname && strpbrk(rv->catalogname, "@/")))
					provider_catalog_index(id);
				goto binding_done;
			}
			if (get_rel_relkind(id) != RELKIND_VIEW)
				provider_catalog_index(id); /* reject unsupported catalogs
											 * explicitly */
			/* Stored-rule RTEs retain native checkAsUser permission entries. */
			if (pstate && pg_class_aclcheck(id, user, ACL_SELECT) != ACLCHECK_OK)
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, rv->relname);
			catalog = table_open(id, AccessShareLock);
			result = CreateTupleDescCopy(RelationGetDescr(catalog));
			rte->dolt_catalog = id;
			if (get_rel_relkind(id) == RELKIND_VIEW)
			{
				if (id >= FirstNormalObjectId)
					unsupported("revision-qualified user views");
				rte->relkind = RELKIND_VIEW;
				rte->relid = id;
			}
			rte->dolt_table = pstrdup("");
			rte->dolt_rowtype = get_rel_type_id(id);

			/* Catalog rows show a branch head's objects by durable OID. */
			if (working)
				rte->dolt_branch = pstrdup(revision);
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
				run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(logical)), NULL, true);
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
				CatalogView *view = working ? provider_branch_view(revision, db) : provider_view(db);
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
binding_done:
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
		run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
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
		handle = CatalogViewLookupObject(provider_branch_view(branch, db), RelationRelationId, name);
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

				run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='index' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(indexname)), NULL, true);
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

	/*
	 * A branch head's table is an ordinary relation with a durable OID; the
	 * executor opens and locks it like any other.
	 */
	{
		Oid			relid = get_typ_typrelid(rte->dolt_rowtype);

		if (CatalogViewIsDurable(relid))
		{
			rte->dolt_branch = NULL;
			rte->dolt_relid = InvalidOid;
			return table_open(relid, RowExclusiveLock);
		}
		return table_open(relid, NoLock);
	}
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
	bool		pushed = false;
	bool		native_found = false;
	char	   *search_path;

	if (CatalogViewIsProvisional(CatalogViewCurrent()))
	{
		if ((!rv->schemaname || dg_system_namespace(rv->schemaname)) && OidIsValid(get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE)))
			return false;
		List	   *path = NIL;
		ListCell   *lc;
		char	   *raw = pstrdup(api_search_path ? api_search_path : "public");

		*result = InvalidOid;
		if (rv->schemaname)
			path = list_make1(pstrdup(rv->schemaname));
		else if (!SplitIdentifierString(raw, ',', &path))
			elog(ERROR, "invalid search_path");
		foreach(lc, path)
		{
			char	   *ns = lfirst(lc);

			if (!strcmp(ns, "$user"))
				ns = GetUserNameFromId(GetUserId(), false);
			*result = CatalogViewFindObject(CatalogViewCurrent(), RelationRelationId,
											quote_qualified_identifier(ns, rv->relname));
			if (OidIsValid(*result))
				break;
		}
		if (!OidIsValid(*result) && !missing_ok)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist in provisional schema", rv->relname)));
		return true;
	}
	if (!dg_catalog_active())
		return false;
	rv = system_alias(rv);
	if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
		return false;
	if (rv->dolt_revision)
		unsupported("revision-qualified native utility lookup");
	search_path = pstrdup(GetConfigOption("search_path", false, false));
	GetUserIdAndSecContext(&user, &security);

	/*
	 * Names on the session's branch resolve against its latest committed
	 * schema, as PostgreSQL resolves names against its catalogs.
	 */
	if (!catalog_current_database())
	{
		PushActiveSnapshot(GetLatestSnapshot());
		pushed = true;
	}
	else if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	*result = InvalidOid;
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db;
		const char *branch;

		nest = begin_api();
		branch = catalog_current_database() ? catalog_current_branch() : dg_current_branch();
		db = catalog_current_database() ? unhex(catalog_current_database()) :
			catalog_working_schema(branch);
		{
			Oid			native;
			char	   *ns = catalog_resolve_name(db, rv, user, &native, search_path, false, true);

			if (OidIsValid(native))
			{
				native_found = true;
				*result = native;
			}
			else if (ns)
				*result = CatalogViewLookupObject(context_view(db, branch), RelationRelationId,
												  quote_qualified_identifier(ns, rv->relname));
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
	return !native_found;
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

	if (!CatalogViewIsLeased(id))
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
		run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
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

/* Bind catalog RTEs loaded from stored rules through the same provider as SQL
 * RangeVars. Preserve aliases and native permission entries from view expansion.
 * Catalog layouts are bootstrap-stable, so existing Vars remain well typed. */
void
dg_catalog_rewrite_relation(RangeTblEntry *rte)
{
	RangeVar   *rv;
	TupleDesc	desc;
	int			i;

	if (rte->rtekind != RTE_RELATION || catalog_api_depth ||
		rangevar_projection_hook || (!rte->dolt_database && !dg_catalog_active()))
		return;
	for (i = 0; i < lengthof(provider_catalogs); i++)
		if (rte->relid == provider_catalogs[i])
			break;
	if (i == lengthof(provider_catalogs))
		return;
	rv = makeRangeVar("pg_catalog", get_rel_name(rte->relid), -1);
	desc = catalog_bind(NULL, rv, rte, false);
	if (!desc)
		elog(ERROR, "could not bind stored catalog reference");
	rte->rtekind = RTE_DOLT;
	rte->inh = false;
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		rte->coltypes = lappend_oid(rte->coltypes, att->atttypid);
		rte->coltypmods = lappend_int(rte->coltypmods, att->atttypmod);
		rte->colcollations = lappend_oid(rte->colcollations, att->attcollation);
	}
	FreeTupleDesc(desc);
}

/* Visibility is a name-resolution question in the object's own schema context,
 * not membership of its synthetic namespace OID in a physical search path. */
bool
dg_catalog_relation_visible(Oid id, const char *name, bool *visible)
{
	CatalogView *view = CatalogViewObjectContext(id);
	DGProvider *provider;
	char	   *database = NULL;
	const char *branch = NULL;
	char	   *path;
	RangeVar   *rv;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;

	if (!view)
		view = CatalogViewCurrent();
	if (catalog_api_depth || rangevar_projection_hook || (!view && !dg_catalog_active()))
		return false;
	for (provider = providers; provider; provider = provider->next)
		if (provider->view == view)
		{
			database = provider->database;
			branch = provider->branch;
		}
	if (view && !database)
		return false;			/* non-production synthetic provider */
	if (!view)
		branch = dg_current_branch();
	path = pstrdup(GetConfigOption("search_path", false, false));
	rv = makeRangeVar(NULL, pstrdup(name), -1);
	GetUserIdAndSecContext(&user, &security);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db;
		Oid			native;
		char	   *ns;

		nest = begin_api();
		db = database ? unhex(database) : read_ref(branch_ref("working")).target;
		ns = catalog_resolve_name(db, rv, user, &native, path, false, true);
		*visible = ns && (OidIsValid(native) ? native == id :
						  CatalogViewLookupObject(context_view(db, branch), RelationRelationId,
												  quote_qualified_identifier(ns, name)) == id);
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}

/* Named types resolve against the same ordered namespace membership as tables.
 * A handled miss is authoritative: never fall back to a physical user type. */
bool
dg_catalog_lookup_type(const char *schema, const char *name, bool temp_ok, Oid *result)
{
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	RangeVar   *rv = makeRangeVar((char *) schema, (char *) name, -1);
	char	   *search_path;

	if (!dg_catalog_active())
		return false;
	search_path = pstrdup(GetConfigOption("search_path", false, false));
	GetUserIdAndSecContext(&user, &security);

	/*
	 * Names on the session's branch resolve against its latest committed
	 * schema, as PostgreSQL resolves names against its catalogs.
	 */
	if (!catalog_current_database())
	{
		PushActiveSnapshot(GetLatestSnapshot());
		pushed = true;
	}
	else if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	*result = InvalidOid;
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db;
		const char *branch;

		nest = begin_api();
		branch = catalog_current_database() ? catalog_current_branch() : dg_current_branch();
		db = catalog_current_database() ? unhex(catalog_current_database()) :
			catalog_working_schema(branch);
		{
			Oid			native;
			char	   *ns = catalog_resolve_name(db, rv, user, &native, search_path, true, temp_ok);

			if (OidIsValid(native))
			{
				*result = native;
			}
			else if (ns)
				*result = CatalogViewLookupObject(context_view(db, branch), TypeRelationId,
												  quote_qualified_identifier(ns, rv->relname));
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
	return true;
}

bool
dg_catalog_type_visible(Oid id, const char *name, bool *visible)
{
	CatalogView *view = CatalogViewObjectContext(id);
	CatalogView *previous = CatalogViewSwitch(view ? view : CatalogViewCurrent());
	bool		handled;
	Oid			found;

	PG_TRY();
	{
		handled = dg_catalog_lookup_type(NULL, name, true, &found);
		if (handled)
			*visible = found == id;
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return handled;
}

/* Compilation scope for argument-free procedural routines. Execution retains
 * its caller's read/write authority; this view only keys cached declarations. */
CatalogView *
dg_catalog_session_view(void)
{
	Oid			user;
	int			security,
				nest;
	CatalogView *result = CatalogViewCurrent();

	if (result || !dg_catalog_active())
		return result;
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		result = provider_branch_view(dg_current_branch(), read_ref(branch_ref("working")).target);
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return result;
}

/*
 * Under a fixed snapshot, a branch lock cannot make the snapshot see a later
 * deletion, so reject roots that changed since the snapshot.
 */
static void
foreign_check_snapshot(Relation rel)
{
	DGState    *visible,
			   *latest;
	bool		same_root;

	if (!IsolationUsesXactSnapshot())
		return;
	visible = dg_load(rel, GetTransactionSnapshot());
	latest = dg_load(rel, GetLatestSnapshot());
	same_root = !memcmp(visible->content.root.bytes, latest->content.root.bytes, PL_HASH_SIZE);
	dg_release(visible);
	dg_release(latest);
	if (!same_root)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("foreign key relation changed since transaction snapshot")));
}

/*
 * Native prolly tables lock referenced rows (FOR KEY SHARE) like heap.
 * Canonical bindings are read through versioned scans that cannot lock rows
 * yet, so their checks take the branch-level resource lock, which excludes
 * concurrent writers of that branch.
 */
void
dg_catalog_foreign_lock(Relation definition)
{
	CatalogView *previous;
	Oid			user;
	int			security;
	Relation	bound;
	const char *name;

	if (!definition->rd_catalogview || CatalogViewIsDurable(RelationGetRelid(definition)))
		return;
	name = CatalogViewObjectIdentity(RelationGetRelid(definition), RelationRelationId);
	GetUserIdAndSecContext(&user, &security);
	previous = CatalogViewSwitch(NULL);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bound = catalog_open_table(dg_current_branch(), name, ACL_UPDATE);
		dg_storage_lock_branch(bound->rd_lockInfo.lockRelId.relId, dg_storage_branch(bound), ExclusiveLock);
		foreign_check_snapshot(bound);
		dg_storage_close_relation(bound);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}

/* Reopen a queued event with its original definition and explicit branch.
 * Data roots use the firing snapshot; schema replacement is never substituted. */
Relation
dg_catalog_trigger_relation(Oid id, const char *branch)
{
	CatalogView *view = CatalogViewObjectContext(id);
	CatalogView *previous = CatalogViewSwitch(NULL);
	DGProvider *provider;
	Relation	result = NULL;
	Oid			user;
	int			security,
				nest;
	const char *name = CatalogViewObjectIdentity(id, RelationRelationId);

	if (!view || !branch)
		elog(ERROR, "queued trigger has no revision binding");
	for (provider = providers; provider && provider->view != view; provider = provider->next)
	{
	}
	if (!provider)
		elog(ERROR, "queued trigger catalog provider disappeared");
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *database,
				   *schema,
				   *content;
		Oid			resource;
		DGStorage  *storage;

		nest = begin_api();
		database = read_ref(psprintf("working/%s", branch)).target;
		run("SELECT t.objchildren[1],t.objchildren[2] FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(database), CStringGetTextDatum(name)), NULL, true);
		if (SPI_processed != 1)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("queued trigger relation disappeared")));
		schema = DatumGetByteaPCopy(cell(0, 1));
		content = DatumGetByteaPCopy(cell(0, 2));
		run("SELECT 1 FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2 AND t.objchildren[1]=$3", 3, VC_TYPES(BYTEAOID, TEXTOID, BYTEAOID), VC_ARGS(PointerGetDatum(unhex(provider->database)), CStringGetTextDatum(name), PointerGetDatum(schema)), NULL, true);
		if (SPI_processed != 1)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("cannot change a relation with pending versioned trigger events")));
		resource = binding_relation(branch, name, schema);
		storage = dg_storage_open(id, branch, name, schema, resource, GetActiveSnapshot());
		result = dg_storage_enable_write(storage, content);
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}
