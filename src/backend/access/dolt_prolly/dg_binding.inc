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
	run("SELECT b.bindrelid FROM pg_dolt_binding b JOIN pg_dolt_storage s ON s.storageid=b.bindrelid WHERE b.bindbranch=$1 AND b.bindname=$2 AND b.bindschema=$3", 3, VC_TYPES(TEXTOID, TEXTOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(branch), CStringGetTextDatum(name), PointerGetDatum(schema)), NULL, true);
	return SPI_processed ? DatumGetObjectId(cell(0, 1)) : InvalidOid;
}

/* Branch administration can reuse an unambiguous, already captured generation.
 * Never synthesize schemas during a query or guess among different generations.
 * Historical branches with no compatible resource remain creatable, but cannot
 * be checked out for writes until a later DDL/bootstrap implementation supplies it. */
static void
binding_prepare(const char *branch, bytea *database, bool required)
{
	int			n,
				i;
	char	  **names;
	bytea	  **schemas;

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
		Oid			relid = binding_relation(branch, names[i], schemas[i]);

		if (OidIsValid(relid))
			continue;
		run("SELECT DISTINCT b.bindrelid FROM pg_dolt_binding b JOIN pg_dolt_storage s ON s.storageid=b.bindrelid WHERE b.bindname=$1 AND b.bindschema=$2", 2, VC_TYPES(TEXTOID, BYTEAOID), VC_ARGS(CStringGetTextDatum(names[i]), PointerGetDatum(schemas[i])), NULL, true);

		/*
		 * An independent resource can hold roots for several schemas. A new
		 * branch starts from its retained descriptor, never another branch's
		 * root.
		 */
		if (SPI_processed != 1)
			run("SELECT min(s.storageid::bigint)::oid FROM pg_dolt_binding b JOIN pg_dolt_storage s ON s.storageid=b.bindrelid WHERE b.bindname=$1 AND NOT s.storageimported HAVING count(*)>0", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(names[i])), NULL, true);
		if (SPI_processed == 1)
			binding_record(branch, names[i], schemas[i], DatumGetObjectId(cell(0, 1)));
		else if (required)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("no unambiguous runtime catalog binding for %s on branch %s", names[i], branch)));
	}
}
