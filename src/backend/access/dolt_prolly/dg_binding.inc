/* Local, MVCC-visible runtime mappings. Canonical membership remains authoritative.
 * These helpers run inside the fixed-search-path, privileged manifest SPI scope. */
static void
binding_record(const char *branch, const char *name, bytea *schema, Oid relid)
{
	Relation	rel;
	SysScanDesc scan;
	Snapshot	snapshot;
	ScanKeyData keys[2];
	HeapTuple	old,
				tuple;
	Datum		values[Natts_pg_dolt_binding];
	bool		nulls[Natts_pg_dolt_binding] = {false};
	bool		isnull;
	bool		changed = true;

	/*
	 * Serialize this prototype's local mapping writers. Readers keep MVCC
	 * views.
	 */
	rel = table_open(DoltBindingRelationId, ShareRowExclusiveLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber,
				F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber,
				F_TEXTEQ, CStringGetTextDatum(name));
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = systable_beginscan(rel, DoltBindingKeyIndexId, true, snapshot, 2, keys);
	old = systable_getnext(scan);
	if (HeapTupleIsValid(old))
	{
		Datum		oldid = heap_getattr(old, Anum_pg_dolt_binding_bindrelid, RelationGetDescr(rel), &isnull);
		Datum		oldschema = heap_getattr(old, Anum_pg_dolt_binding_bindschema, RelationGetDescr(rel), &isnull);

		changed = DatumGetObjectId(oldid) != relid || !same(DatumGetByteaPP(oldschema), schema);
	}
	if (changed)
	{
		values[Anum_pg_dolt_binding_bindrelid - 1] = ObjectIdGetDatum(relid);
		values[Anum_pg_dolt_binding_bindbranch - 1] = CStringGetTextDatum(branch);
		values[Anum_pg_dolt_binding_bindname - 1] = CStringGetTextDatum(name);
		values[Anum_pg_dolt_binding_bindschema - 1] = PointerGetDatum(schema);
		tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		if (HeapTupleIsValid(old))
			CatalogTupleUpdate(rel, &old->t_self, tuple);
		else
			CatalogTupleInsert(rel, tuple);
		heap_freetuple(tuple);
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, NoLock);
	if (changed)
		CommandCounterIncrement();
}

static Oid
binding_relation(const char *branch, const char *name, bytea *schema)
{
	run("SELECT b.bindrelid FROM pg_dolt_binding b JOIN pg_class c ON c.oid=b.bindrelid WHERE b.bindbranch=$1 AND b.bindname=$2 AND b.bindschema=$3", 3, VC_TYPES(TEXTOID, TEXTOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(branch), CStringGetTextDatum(name), PointerGetDatum(schema)), NULL, true);
	return SPI_processed ? DatumGetObjectId(cell(0, 1)) : InvalidOid;
}

/* A fresh branch has no local roots: its retained HEAD initializes contents.
 * Any schema-compatible table generation can therefore host it; indexes must
 * belong to the selected table. Existing branches require an unambiguous mapping.
 * Branch administration can reuse an unambiguous, already captured generation.
 * Never synthesize schemas during a query or guess among different generations.
 * Historical branches with no compatible resource remain creatable, but cannot
 * be checked out for writes until a later DDL/bootstrap implementation supplies it. */
static void
binding_prepare(const char *branch, bytea *database, bool required, bool fresh)
{
	int			n,
				i;
	char	  **names;
	bytea	  **schemas;

	run("SELECT convert_from(t.objpayload,'UTF8')::jsonb->>'name',t.objchildren[1] FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index') ORDER BY t.objkind DESC", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(database)), NULL, true);
	n = SPI_processed;
	names = palloc(sizeof(char *) * Max(n, 1));
	schemas = palloc(sizeof(bytea *) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		schemas[i] = DatumGetByteaPCopy(cell(i, 2));
	}
	for (i = 0; i < n; i++)
	{
		Oid			relid = binding_relation(branch, names[i], schemas[i]);

		if (OidIsValid(relid))
			continue;

		/*
		 * SPI takes a current command snapshot to see parent mappings just
		 * inserted.
		 */
		run("SELECT DISTINCT b.bindrelid FROM pg_dolt_binding b JOIN pg_class c ON c.oid=b.bindrelid WHERE b.bindname=$1 AND b.bindschema=$2 AND (c.relkind='r' OR EXISTS (SELECT 1 FROM pg_index x JOIN pg_dolt_binding parent ON parent.bindrelid=x.indrelid WHERE x.indexrelid=c.oid AND parent.bindbranch=$3)) ORDER BY b.bindrelid", 3, VC_TYPES(TEXTOID, BYTEAOID, TEXTOID), VC_ARGS(CStringGetTextDatum(names[i]), PointerGetDatum(schemas[i]), CStringGetTextDatum(branch)), NULL, false);
		if (SPI_processed == 1 || (fresh && SPI_processed > 0))
			binding_record(branch, names[i], schemas[i], DatumGetObjectId(cell(0, 1)));
		else if (required)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("no unambiguous runtime catalog binding for %s on branch %s", names[i], branch)));
	}
}

/* Namespace indirection is immutable for each allocated runtime generation. */
Oid
dg_catalog_namespace(Oid physical)
{
	HeapTuple	tuple;
	Oid			logical = physical;

	if (!IsUnderPostmaster || !OidIsValid(physical))
		return physical;
	tuple = SearchSysCache1(DOLTNAMESPACE, ObjectIdGetDatum(physical));
	if (HeapTupleIsValid(tuple))
	{
		logical = ((Form_pg_dolt_namespace) GETSTRUCT(tuple))->logicalnsp;
		ReleaseSysCache(tuple);
	}
	return logical;
}

static char *
binding_name(Oid id)
{
	return quote_qualified_identifier(get_namespace_name(dg_catalog_namespace(get_rel_namespace(id))), get_rel_name(id));
}

static Oid
binding_create_namespace(Oid logical, Oid owner)
{
	char	   *name;
	Oid			id;
	Relation	rel;
	HeapTuple	tuple;
	Datum		values[Natts_pg_dolt_namespace];
	bool		nulls[Natts_pg_dolt_namespace] = {false};

	do
	{
		name = psprintf("pg_dolt_runtime_%u", GetNewObjectId());
	}
	while (OidIsValid(get_namespace_oid(name, true)));
	id = NamespaceCreate(name, owner, false);
	values[Anum_pg_dolt_namespace_runtimensp - 1] = ObjectIdGetDatum(id);
	values[Anum_pg_dolt_namespace_logicalnsp - 1] = ObjectIdGetDatum(logical);
	rel = table_open(DoltNamespaceRelationId, RowExclusiveLock);
	tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
	table_close(rel, NoLock);
	CommandCounterIncrement();
	return id;
}

/* Enumerate canonical members, then select their exact branch runtime binding. */
static List *
binding_members(bytea *database, const char *branch)
{
	List	   *result = NIL;
	char	  **names;
	bytea	  **schemas;
	int			n,
				i;

	run("SELECT convert_from(t.objpayload,'UTF8')::jsonb->>'name',t.objchildren[1] FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index')", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(database)), NULL, true);
	n = SPI_processed;
	names = palloc(sizeof(char *) * Max(n, 1));
	schemas = palloc(sizeof(bytea *) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		schemas[i] = DatumGetByteaPCopy(cell(i, 2));
	}
	for (i = 0; i < n; i++)
	{
		Oid			id = binding_relation(branch, names[i], schemas[i]);

		if (!OidIsValid(id))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", names[i], branch)));
		result = lappend_oid(result, id);
	}
	return result;
}

/* Supply only the OID: RangeVarGetRelidExtended retains locks and callbacks. */
bool
dg_catalog_resolve(const RangeVar *rv, bool missing_ok, Oid *id)
{
	List	   *path;
	ListCell   *lc;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	bool		handled = false;
	Oid			result = InvalidOid;

	if (!IsUnderPostmaster || !IsTransactionState() || rangevar_projection_hook ||
		rv->relpersistence == RELPERSISTENCE_TEMP || rv->dolt_revision)
		return false;
	if (rv->catalogname && strcmp(rv->catalogname, get_database_name(MyDatabaseId)))
		return false;
	if (rv->schemaname)
	{
		Oid			ns = LookupExplicitNamespace(rv->schemaname, true);

		if (!OidIsValid(ns))
			return false;
		path = list_make1_oid(ns);
	}
	else
		path = fetch_search_path(true);
	/* Catalog/temporary lookups must not recurse into the provider. */
	foreach(lc, path)
	{
		Oid			ns = lfirst_oid(lc);
		char	   *name = get_namespace_name(ns);

		if (dg_catalog_namespace(ns) != ns)
		{
			if (catalog_creating)
				return false;
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("private runtime namespaces cannot be referenced directly")));
		}
		if (name && (!strncmp(name, "pg_", 3) || !strcmp(name, "information_schema")))
		{
			if (OidIsValid(get_relname_relid(rv->relname, ns)))
				return false;
		}
		else
			break;
	}
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

		nest = begin_api();
		db = catalog_database(dg_current_branch());
		if (db)
		{
			handled = true;
			foreach(lc, path)
			{
				Oid			ns = lfirst_oid(lc);
				char	   *nsname = get_namespace_name(ns);
				char	   *logical;
				Oid			physical;

				if (!nsname)
					continue;
				if (dg_catalog_namespace(ns) != ns)
					ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("private runtime namespaces cannot be referenced directly")));
				physical = get_relname_relid(rv->relname, ns);
				/* Preserve ordinary AMs and temporary/system lookup behavior. */
				if (OidIsValid(physical) && get_rel_relam(physical) != DOLT_PROLLY_TABLE_AM_OID && get_rel_relam(physical) != DOLT_PROLLY_INDEX_AM_OID)
				{
					result = physical;
					break;
				}
				logical = quote_qualified_identifier(nsname, rv->relname);
				run("SELECT t.objchildren[1] FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index') AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(logical)), NULL, true);
				if (SPI_processed)
				{
					result = binding_relation(dg_current_branch(), logical, DatumGetByteaPCopy(cell(0, 1)));
					if (!OidIsValid(result))
						ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", logical, dg_current_branch())));
					break;
				}
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
	if (handled && !OidIsValid(result) && !missing_ok)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", rv->relname, dg_current_branch())));
	*id = result;
	return handled;
}

/* Native index naming also observes the logical namespace, not just its slot. */
bool
dg_catalog_name_exists(Oid physical, const char *name)
{
	Oid			logical = dg_catalog_namespace(physical);
	RangeVar   *rv;
	Oid			id;

	if (logical == physical)
		return false;
	rv = makeRangeVar(get_namespace_name(logical), pstrdup(name), -1);
	return dg_catalog_resolve(rv, true, &id) && OidIsValid(id);
}
