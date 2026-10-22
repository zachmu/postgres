/*
 * A canonical definition and a storage resource are different objects.
 * Resource records contain locators and access-method properties, never columns,
 * names, types or constraints. The transaction snapshot selects the binding.
 *
 * For now native capture registers existing permanent relation files. A future
 * canonical CREATE can allocate the same resource without a physical schema row.
 */
#include "dg.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/catalogview.h"
#include "catalog/catalog.h"
#include "catalog/storage.h"
#include "catalog/pg_am_d.h"
#include "utils/syscache.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_binding.h"
#include "catalog/pg_dolt_object.h"
#include "catalog/pg_dolt_storage.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"

/* This descriptor is private to its consumer, not an entry in native relcache. */
struct DGStorage
{
	RelationData relation;
	FormData_pg_class form;
	Relation	definition;
	char	   *branch;
	Snapshot	snapshot;
	MemoryContextCallback cleanup;
};

static bool
same_schema(bytea *a, bytea *b)
{
	return VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b) &&
		!memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a));
}

/* Called while capture holds the source relation lock. */
void
dg_storage_record(Relation source)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	old,
				tuple;
	Datum		values[Natts_pg_dolt_storage];
	bool		nulls[Natts_pg_dolt_storage] = {false};
	bool		changed = true;
	Snapshot	snapshot;
	Oid			id = RelationGetRelid(source);

	if (CatalogViewObjectContext(id) || source->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
		source->rd_locator.dbOid != MyDatabaseId || !RelFileNumberIsValid(source->rd_locator.relNumber))
		elog(ERROR, "invalid physical source for prolly storage registration");
	catalog = table_open(DoltStorageRelationId, ShareRowExclusiveLock);
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	ScanKeyInit(&key, Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(id));
	scan = systable_beginscan(catalog, DoltStorageKeyIndexId, true, snapshot, 1, &key);
	old = systable_getnext(scan);
	if (HeapTupleIsValid(old))
	{
		Form_pg_dolt_storage form = (Form_pg_dolt_storage) GETSTRUCT(old);

		if (!form->storageimported)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource identity belongs to an independent allocation")));

		changed = form->storagespc != source->rd_locator.spcOid || form->storagedb != source->rd_locator.dbOid ||
			form->storagefile != source->rd_locator.relNumber || form->storageam != source->rd_rel->relam ||
			form->storagekind != source->rd_rel->relkind;
	}
	if (changed)
	{
		values[Anum_pg_dolt_storage_storageid - 1] = ObjectIdGetDatum(id);
		values[Anum_pg_dolt_storage_storagespc - 1] = ObjectIdGetDatum(source->rd_locator.spcOid);
		values[Anum_pg_dolt_storage_storagedb - 1] = ObjectIdGetDatum(source->rd_locator.dbOid);
		values[Anum_pg_dolt_storage_storagefile - 1] = ObjectIdGetDatum(source->rd_locator.relNumber);
		values[Anum_pg_dolt_storage_storageam - 1] = ObjectIdGetDatum(source->rd_rel->relam);
		values[Anum_pg_dolt_storage_storagekind - 1] = CharGetDatum(source->rd_rel->relkind);
		values[Anum_pg_dolt_storage_storageimported - 1] = BoolGetDatum(true);
		tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
		if (HeapTupleIsValid(old))
			CatalogTupleUpdate(catalog, &old->t_self, tuple);
		else
			CatalogTupleInsert(catalog, tuple);
		heap_freetuple(tuple);
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(catalog, NoLock);
	if (changed)
		CommandCounterIncrement();
}

/* CREATE's storage primitive: no pg_class, pg_attribute or pg_type writes.
 * The caller must publish canonical membership and a branch binding in the same
 * transaction. Native pending-delete machinery owns file cleanup on abort. */
Oid
dg_storage_allocate(Relation definition, const char *branch)
{
	Relation	catalog;
	Relation	resource = palloc0(sizeof(RelationData));
	FormData_pg_class form;
	Datum		values[Natts_pg_dolt_storage];
	bool		nulls[Natts_pg_dolt_storage] = {false};
	HeapTuple	tuple;
	Oid			id;

	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot allocate storage in a read-only transaction")));
	if (!definition->rd_catalogview ||
		!((definition->rd_rel->relkind == RELKIND_RELATION && definition->rd_rel->relam == DOLT_PROLLY_TABLE_AM_OID) ||
		  (definition->rd_rel->relkind == RELKIND_INDEX && definition->rd_rel->relam == DOLT_PROLLY_INDEX_AM_OID)))
		elog(ERROR, "storage allocation requires a canonical prolly definition");
	catalog = table_open(DoltStorageRelationId, ShareRowExclusiveLock);
	do
	{
		id = GetNewOidWithIndex(catalog, DoltStorageKeyIndexId, Anum_pg_dolt_storage_storageid);
	}
	while (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(id)));
	LockRelationOid(id, AccessExclusiveLock);
	*resource = *definition;
	form = *definition->rd_rel;
	resource->rd_rel = &form;
	resource->rd_smgr = NULL;
	resource->rd_locator.spcOid = MyDatabaseTableSpace;
	resource->rd_locator.dbOid = MyDatabaseId;
	resource->rd_locator.relNumber = GetNewRelFileNumber(MyDatabaseTableSpace, NULL, RELPERSISTENCE_PERMANENT);
	resource->rd_lockInfo.lockRelId.relId = id;
	resource->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
	values[Anum_pg_dolt_storage_storageid - 1] = ObjectIdGetDatum(id);
	values[Anum_pg_dolt_storage_storagespc - 1] = ObjectIdGetDatum(resource->rd_locator.spcOid);
	values[Anum_pg_dolt_storage_storagedb - 1] = ObjectIdGetDatum(resource->rd_locator.dbOid);
	values[Anum_pg_dolt_storage_storagefile - 1] = ObjectIdGetDatum(resource->rd_locator.relNumber);
	values[Anum_pg_dolt_storage_storageam - 1] = ObjectIdGetDatum(form.relam);
	values[Anum_pg_dolt_storage_storagekind - 1] = CharGetDatum(form.relkind);
	values[Anum_pg_dolt_storage_storageimported - 1] = BoolGetDatum(false);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	CatalogTupleInsert(catalog, tuple);
	heap_freetuple(tuple);
	table_close(catalog, NoLock);
	(void) RelationCreateStorage(resource->rd_locator, RELPERSISTENCE_PERMANENT, true);
	PG_TRY();
	{
		dg_initialize_resource(resource, branch);
	}
	PG_FINALLY();
	{
		RelationCloseSmgr(resource);
	}
	PG_END_TRY();
	CommandCounterIncrement();
	return id;
}

static void
storage_cleanup(void *arg)
{
	DGStorage  *storage = arg;

	/*
	 * The resource owner releases definition pins on ERROR. Only this private
	 * descriptor owns the smgr pin, so memory-context cleanup handles it.
	 */
	RelationCloseSmgr(&storage->relation);
}

DGStorage *
dg_storage_open(Oid definition, const char *branch, const char *name,
				bytea *schema, Oid resource, Snapshot snapshot)
{
	DGStorage  *storage = palloc0(sizeof(*storage));
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tuple;
	bool		isnull;
	bytea	   *stored;
	FormData_pg_dolt_storage form;
	LockRelId	lock = {resource, MyDatabaseId};

	if (!CatalogViewObjectContext(definition) || !OidIsValid(resource) ||
		resource >= FirstCatalogViewObjectId || !snapshot || !IsMVCCSnapshot(snapshot))
		elog(ERROR, "invalid canonical storage binding");

	/*
	 * Use the stable resource identity BEFORE reading its locator. Native
	 * physical DDL and ordinary AM writers currently use this same lock
	 * identity.
	 */
	LockRelationId(&lock, AccessShareLock);
	catalog = table_open(DoltBindingRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(catalog, DoltBindingKeyIndexId, true, snapshot, 2, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime storage binding unavailable for %s on branch %s", name, branch)));
	stored = DatumGetByteaPP(heap_getattr(tuple, Anum_pg_dolt_binding_bindschema, RelationGetDescr(catalog), &isnull));
	if (((Form_pg_dolt_binding) GETSTRUCT(tuple))->bindrelid != resource || !same_schema(stored, schema))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime storage binding changed for %s on branch %s", name, branch)));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	catalog = table_open(DoltStorageRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_dolt_storage_storageid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(resource));
	scan = systable_beginscan(catalog, DoltStorageKeyIndexId, true, snapshot, 1, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource %u is unavailable", resource)));
	form = *(Form_pg_dolt_storage) GETSTRUCT(tuple);
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	storage->definition = relation_open(definition, NoLock);
	if (form.storagedb != MyDatabaseId || !OidIsValid(form.storagespc) || !RelFileNumberIsValid(form.storagefile) ||
		form.storageam != storage->definition->rd_rel->relam || form.storagekind != storage->definition->rd_rel->relkind)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource properties do not match canonical definition")));

	/*
	 * Imported resources still follow native physical DDL until that DDL is
	 * routed through canonical publication. Validate this bridge, never adopt
	 * its definition. Independently allocated resources have no physical
	 * schema row.
	 */
	if (form.storageimported)
	{
		Relation	source = try_relation_open(resource, NoLock);
		TupleDesc	a,
					b = RelationGetDescr(storage->definition);
		RelFileLocator locator = {form.storagespc, form.storagedb, form.storagefile};
		bool		compatible;

		if (!source)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding unavailable for %s on branch %s", name, branch)));
		a = RelationGetDescr(source);
		compatible = a->natts == b->natts && RelFileLocatorEquals(source->rd_locator, locator) && source->rd_rel->relam == form.storageam;
		for (int i = 0; compatible && i < a->natts; i++)
		{
			Form_pg_attribute x = TupleDescAttr(a, i),
						y = TupleDescAttr(b, i);

			compatible = !x->attisdropped && x->atttypid == y->atttypid && x->atttypmod == y->atttypmod && x->attcollation == y->attcollation && !strcmp(NameStr(x->attname), NameStr(y->attname));
		}
		relation_close(source, NoLock);
		if (!compatible)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unpublished branch schema changes are not supported yet")));
	}
	storage->relation = *storage->definition;
	storage->form = *storage->definition->rd_rel;
	storage->relation.rd_rel = &storage->form;
	storage->relation.rd_smgr = NULL;
	storage->relation.rd_locator.spcOid = form.storagespc;
	storage->relation.rd_locator.dbOid = form.storagedb;
	storage->relation.rd_locator.relNumber = form.storagefile;
	storage->relation.rd_lockInfo.lockRelId = lock;
	storage->branch = pstrdup(branch);
	storage->snapshot = RegisterSnapshot(snapshot);
	storage->cleanup.func = storage_cleanup;
	storage->cleanup.arg = storage;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &storage->cleanup);
	if (!smgrexists(RelationGetSmgr(&storage->relation), MAIN_FORKNUM))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("storage resource file is unavailable")));
	return storage;
}

/* Only this checked entrypoint admits a canonical descriptor to page storage.
 * There is no ambient authority to write a retained/schema-only handle. */
DGState *
dg_storage_load(DGStorage *storage)
{
	return dg_load_bound(&storage->relation, storage->snapshot, storage->branch);
}

void
dg_storage_close(DGStorage *storage)
{
	RelationCloseSmgr(&storage->relation);
	UnregisterSnapshot(storage->snapshot);
	relation_close(storage->definition, NoLock);
	storage->definition = NULL;
	/* Keep the stable resource lock to transaction end, as native scans do. */
}
