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
	 * Mapping mutations are protected by their tuple/unique-index locks.
	 * Different branches must not hold a conflicting catalog-wide lock.
	 */
	rel = table_open(DoltBindingRelationId, RowExclusiveLock);
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

	/* Every bound object on a branch head has a durable OID. */
	(void) dg_identity_ensure(branch, DOLT_IDENTITY_RELATION, name);
	run("SELECT storagekind FROM pg_catalog.pg_dolt_storage WHERE storageid=$1", 1, VC_TYPES(OIDOID), VC_ARGS(ObjectIdGetDatum(relid)), NULL, false);
	if (SPI_processed == 1 && DatumGetChar(cell(0, 1)) == RELKIND_RELATION)
	{
		(void) dg_identity_ensure(branch, DOLT_IDENTITY_ROWTYPE, name);
		(void) dg_identity_ensure(branch, DOLT_IDENTITY_ARRAYTYPE, name);
	}
}

/*
 * The resource the local mapping of |name| on |branch| names for retained
 * schema |schema|, under |snapshot|, if that resource exists: what
 * binding_relation finds, read natively. InvalidOid otherwise.
 */
static Oid
binding_lookup(const char *branch, const char *name, bytea *schema, Snapshot snapshot)
{
	Relation	rel = table_open(DoltBindingRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			resource = InvalidOid;

	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(rel, DoltBindingKeyIndexId, true, snapshot, 2, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		bool		isnull;
		Datum		stored = heap_getattr(tuple, Anum_pg_dolt_binding_bindschema, RelationGetDescr(rel), &isnull);

		if (schema && !isnull && same(DatumGetByteaPP(stored), schema))
			resource = ((Form_pg_dolt_binding) GETSTRUCT(tuple))->bindrelid;
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	if (!OidIsValid(resource))
		return InvalidOid;

	/* The resource itself must still exist. */
	rel = table_open(DoltStorageRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(resource));
	scan = systable_beginscan(rel, DoltStorageKeyIndexId, true, snapshot, 1, keys);
	if (!HeapTupleIsValid(systable_getnext(scan)))
		resource = InvalidOid;
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return resource;
}

/* The storage resource bound to |name| on |branch|, whatever its schema. */
static Oid
binding_storage(const char *branch, const char *name)
{
	Relation	rel = table_open(DoltBindingRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			resource = InvalidOid;

	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(rel, DoltBindingKeyIndexId, true,
							  ActiveSnapshotSet() ? GetActiveSnapshot() : NULL, 2, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		resource = ((Form_pg_dolt_binding) GETSTRUCT(tuple))->bindrelid;
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return resource;
}

static Oid
binding_relation(const char *branch, const char *name, bytea *schema)
{
	return binding_lookup(branch, name, schema, GetActiveSnapshot());
}

/* Branch administration selects an allocation; the retained descriptor supplies
 * its schema and initial data. Prefer an exact schema binding, otherwise reuse
 * a same-kind resource. Never derive the definition from that resource's current
 * physical catalog row. Branches without a reusable resource remain read-only. */
static void
binding_prepare(const char *branch, bytea *database, bool required)
{
	int			n,
				i;
	char	  **names,
			  **kinds;
	bytea	  **schemas;

	{
		int			count;
		DGMember   *listed = dg_database_members(database, &count);

		names = palloc(sizeof(char *) * Max(count, 1));
		schemas = palloc(sizeof(bytea *) * Max(count, 1));
		kinds = palloc_array(char *, Max(count, 1));
		n = 0;
		for (i = 0; i < count; i++)
		{
			if (strcmp(listed[i].kind, "table") && strcmp(listed[i].kind, "index"))
				continue;
			names[n] = (char *) listed[i].name;
			schemas[n] = object_child(listed[i].hash, 1);
			kinds[n++] = (char *) listed[i].kind;
		}
	}
	for (i = 0; i < n; i++)
	{
		Oid			relid = binding_relation(branch, names[i], schemas[i]);

		if (OidIsValid(relid))
			continue;
		run("SELECT DISTINCT b.bindrelid FROM pg_dolt_binding b JOIN pg_dolt_storage s ON s.storageid=b.bindrelid WHERE b.bindname=$1 AND b.bindschema=$2", 2, VC_TYPES(TEXTOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(names[i]), PointerGetDatum(schemas[i])), NULL, true);

		/*
		 * A resource can hold roots for several schemas, including an
		 * imported allocation whose physical bridge validates only its
		 * locator and AM. A new branch starts from its retained descriptor,
		 * never another branch's root.
		 */
		if (SPI_processed != 1)
			run("SELECT min(s.storageid::bigint)::oid FROM pg_dolt_binding b JOIN pg_dolt_storage s ON s.storageid=b.bindrelid WHERE b.bindname=$1 AND s.storagekind=CASE WHEN $2='table' THEN 'r'::\"char\" ELSE 'i'::\"char\" END HAVING count(*)>0", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(names[i]), CStringGetTextDatum(kinds[i])), NULL, true);
		if (SPI_processed == 1)
			binding_record(branch, names[i], schemas[i], DatumGetObjectId(cell(0, 1)));
		else if (required)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("no unambiguous runtime catalog binding for %s on branch %s", names[i], branch)));
	}
}
