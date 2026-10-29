/*-------------------------------------------------------------------------
 *
 * dg_identity.c
 *	  Durable OIDs of prolly objects on branch heads.
 *
 * pg_dolt_identity gives each table and index on each branch, and each table's
 * row and array types, one OID that every backend and every hot standby agree
 * on. Locks, invalidation messages and plans name objects by these OIDs, so a
 * branch's objects never share an OID with another branch's.
 *
 * OIDs come from a reserved range, allocated from a shared counter and probed
 * against the catalog's unique index. The counter restarts from the largest
 * OID in use and wraps within the range, so OIDs of dropped objects are reused.
 *
 * src/backend/access/dolt_prolly/dg_identity.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/doltcatalog.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalogview.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_binding.h"
#include "catalog/pg_dolt_identity.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/dsm_registry.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

typedef struct IdentityCounter
{
	pg_atomic_uint32 next;		/* zero until initialized */
} IdentityCounter;

static IdentityCounter *counter;

static void identity_retain(const char *branch, List *members);

static void
counter_init(void *ptr)
{
	pg_atomic_init_u32(&((IdentityCounter *) ptr)->next, 0);
}

/* The largest durable OID in use, or the range start minus one. */
static Oid
max_identity(Relation catalog)
{
	Relation	index = index_open(DoltIdentityOidIndexId, AccessShareLock);
	SysScanDesc scan = systable_beginscan_ordered(catalog, index, SnapshotAny, 0, NULL);
	HeapTuple	tuple = systable_getnext_ordered(scan, BackwardScanDirection);
	Oid			max = FirstDurableObjectId - 1;

	if (HeapTupleIsValid(tuple))
		max = ((Form_pg_dolt_identity) GETSTRUCT(tuple))->identoid;
	systable_endscan_ordered(scan);
	index_close(index, AccessShareLock);
	return max;
}

static bool
identity_exists(Relation catalog, Oid oid)
{
	ScanKeyData key;
	SysScanDesc scan;
	bool		exists;

	ScanKeyInit(&key, Anum_pg_dolt_identity_identoid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(oid));
	scan = systable_beginscan(catalog, DoltIdentityOidIndexId, true, SnapshotAny, 1, &key);
	exists = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	return exists;
}

static Oid
allocate_identity(Relation catalog)
{
	bool		found;

	if (!counter)
		counter = GetNamedDSMSegment("dolt durable identities", sizeof(IdentityCounter), counter_init, &found);
	if (pg_atomic_read_u32(&counter->next) == 0)
	{
		uint32		expected = 0;
		Oid			start = max_identity(catalog) + 1;

		if (start >= FirstCatalogViewObjectId)
			start = FirstDurableObjectId;
		(void) pg_atomic_compare_exchange_u32(&counter->next, &expected, start);
	}
	for (uint64 tries = 0; tries < (uint64) (FirstCatalogViewObjectId - FirstDurableObjectId); tries++)
	{
		Oid			oid = pg_atomic_fetch_add_u32(&counter->next, 1);

		if (oid >= FirstCatalogViewObjectId || oid < FirstDurableObjectId)
		{
			uint32		expected = oid + 1;

			(void) pg_atomic_compare_exchange_u32(&counter->next, &expected, FirstDurableObjectId);
			continue;
		}
		if (!identity_exists(catalog, oid))
			return oid;
	}
	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("durable object identifiers are exhausted")));
	pg_unreachable();
}

static void
name_keys(ScanKeyData *keys, const char *branch, char kind, const char *name)
{
	ScanKeyInit(&keys[0], Anum_pg_dolt_identity_identbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_identity_identkind, BTEqualStrategyNumber, F_CHAREQ, CharGetDatum(kind));
	ScanKeyInit(&keys[2], Anum_pg_dolt_identity_identname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
}

/* The durable OID of |kind| object |name| on |branch|, or InvalidOid. */
Oid
dg_identity_lookup(const char *branch, char kind, const char *name)
{
	Relation	catalog = table_open(DoltIdentityRelationId, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			oid = InvalidOid;

	name_keys(keys, branch, kind, name);
	scan = systable_beginscan(catalog, DoltIdentityNameIndexId, true, NULL, 3, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		oid = ((Form_pg_dolt_identity) GETSTRUCT(tuple))->identoid;
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return oid;
}

/* The branch and name of durable OID |oid|; false if no branch defines it. */
bool
dg_identity_describe(Oid oid, char **branch, char *kind, char **name)
{
	Relation	catalog = table_open(DoltIdentityRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	ScanKeyInit(&key, Anum_pg_dolt_identity_identoid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(oid));
	scan = systable_beginscan(catalog, DoltIdentityOidIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		bool		isnull;

		*branch = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_identity_identbranch, RelationGetDescr(catalog), &isnull));
		*name = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_identity_identname, RelationGetDescr(catalog), &isnull));
		*kind = ((Form_pg_dolt_identity) GETSTRUCT(tuple))->identkind;
		found = true;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return found;
}

/* Ensure |kind| object |name| on |branch| has a durable OID; return it. */
Oid
dg_identity_ensure(const char *branch, char kind, const char *name)
{
	Oid			oid = dg_identity_lookup(branch, kind, name);
	Relation	catalog;
	Datum		values[Natts_pg_dolt_identity];
	bool		nulls[Natts_pg_dolt_identity] = {false};
	HeapTuple	tuple;

	if (OidIsValid(oid))
		return oid;
	catalog = table_open(DoltIdentityRelationId, RowExclusiveLock);
	oid = allocate_identity(catalog);
	values[Anum_pg_dolt_identity_identoid - 1] = ObjectIdGetDatum(oid);
	values[Anum_pg_dolt_identity_identkind - 1] = CharGetDatum(kind);
	values[Anum_pg_dolt_identity_identbranch - 1] = CStringGetTextDatum(branch);
	values[Anum_pg_dolt_identity_identname - 1] = CStringGetTextDatum(name);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	CatalogTupleInsert(catalog, tuple);
	heap_freetuple(tuple);
	table_close(catalog, RowExclusiveLock);
	CacheInvalidateRelcacheByRelid(DoltIdentityRelationId);
	CommandCounterIncrement();
	return oid;
}

/*
 * Invalidate every relation identity of |branch|, and with |remove| delete all
 * of the branch's identities.
 */
static void
branch_identities(const char *branch, bool remove)
{
	Relation	catalog = table_open(DoltIdentityRelationId, remove ? RowExclusiveLock : AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	/* Include identities this transaction created since the catalog snapshot. */
	InvalidateCatalogSnapshot();
	ScanKeyInit(&key, Anum_pg_dolt_identity_identbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = systable_beginscan(catalog, DoltIdentityNameIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_dolt_identity form = (Form_pg_dolt_identity) GETSTRUCT(tuple);

		if (form->identkind == DOLT_IDENTITY_RELATION)
			CacheInvalidateRelcacheInDatabase(form->identoid);
		if (remove)
		{
			if (form->identkind == DOLT_IDENTITY_RELATION)
				RemoveStatistics(form->identoid, 0);
			CatalogTupleDelete(catalog, &tuple->t_self);
		}
	}
	systable_endscan(scan);
	table_close(catalog, remove ? RowExclusiveLock : AccessShareLock);
}

/*
 * A branch's working schema changed: every backend, and every standby, must
 * rebuild its descriptors of the branch's objects from the new schema.
 */
void
dg_identity_invalidate_branch(const char *branch)
{
	branch_identities(branch, false);
}

/* The branch is gone: its objects lose their OIDs. */
void
dg_identity_delete_branch(const char *branch)
{
	branch_identities(branch, true);
	CacheInvalidateRelcacheByRelid(DoltIdentityRelationId);
	CommandCounterIncrement();
}

/*
 * Make |branch|'s identities name exactly |members|, its schema's tables and
 * indexes: delete the others' and create missing ones, with row and array
 * types for |tables|.
 */
void
dg_identity_sync(const char *branch, List *members, List *tables)
{
	ListCell   *lc;

	identity_retain(branch, members);
	foreach(lc, members)
		(void) dg_identity_ensure(branch, DOLT_IDENTITY_RELATION, lfirst(lc));
	foreach(lc, tables)
	{
		(void) dg_identity_ensure(branch, DOLT_IDENTITY_ROWTYPE, lfirst(lc));
		(void) dg_identity_ensure(branch, DOLT_IDENTITY_ARRAYTYPE, lfirst(lc));
	}
}

/* Delete the identities of |branch|'s objects whose names are not in |members|. */
static void
identity_retain(const char *branch, List *members)
{
	Relation	catalog = table_open(DoltIdentityRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		removed = false;

	InvalidateCatalogSnapshot();
	ScanKeyInit(&key, Anum_pg_dolt_identity_identbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = systable_beginscan(catalog, DoltIdentityNameIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		char	   *name = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_identity_identname, RelationGetDescr(catalog), &isnull));
		ListCell   *lc;

		foreach(lc, members)
			if (!strcmp(lfirst(lc), name))
				break;
		if (lc)
			continue;
		if (((Form_pg_dolt_identity) GETSTRUCT(tuple))->identkind == DOLT_IDENTITY_RELATION)
		{
			CacheInvalidateRelcacheInDatabase(((Form_pg_dolt_identity) GETSTRUCT(tuple))->identoid);
			RemoveStatistics(((Form_pg_dolt_identity) GETSTRUCT(tuple))->identoid, 0);
		}
		CatalogTupleDelete(catalog, &tuple->t_self);
		removed = true;
	}
	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
	if (removed)
	{
		CacheInvalidateRelcacheByRelid(DoltIdentityRelationId);
		CommandCounterIncrement();
	}
}

/* The durable OIDs of the relations of |branch|, or of every branch if NULL. */
List *
dg_identity_relations(const char *branch)
{
	Relation	catalog = table_open(DoltIdentityRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	if (branch)
		ScanKeyInit(&key, Anum_pg_dolt_identity_identbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = systable_beginscan(catalog, DoltIdentityNameIndexId, true, NULL, branch ? 1 : 0, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_dolt_identity form = (Form_pg_dolt_identity) GETSTRUCT(tuple);

		if (form->identkind == DOLT_IDENTITY_RELATION)
			result = lappend_oid(result, form->identoid);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return result;
}

/* The durable OID of the relation a pg_dolt_binding row supplies storage for. */
Oid
dg_binding_identity(Relation binding, HeapTuple tuple)
{
	bool		isnull;
	char	   *branch = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_binding_bindbranch, RelationGetDescr(binding), &isnull));
	char	   *name = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_binding_bindname, RelationGetDescr(binding), &isnull));

	return dg_identity_lookup(branch, DOLT_IDENTITY_RELATION, name);
}

/*
 * The storage resource of durable relation |oid|, from its branch's binding
 * under a fresh catalog snapshot; InvalidOid if it has none.
 */
Oid
dg_identity_storage(Oid oid)
{
	char	   *branch,
			   *name;
	char		kind;
	Relation	catalog;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			storage = InvalidOid;

	if (!dg_identity_describe(oid, &branch, &kind, &name) || kind != DOLT_IDENTITY_RELATION)
		return InvalidOid;
	catalog = table_open(DoltBindingRelationId, AccessShareLock);
	InvalidateCatalogSnapshot();
	ScanKeyInit(&keys[0], Anum_pg_dolt_binding_bindbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	ScanKeyInit(&keys[1], Anum_pg_dolt_binding_bindname, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(name));
	scan = systable_beginscan(catalog, DoltBindingKeyIndexId, true, NULL, 2, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		storage = ((Form_pg_dolt_binding) GETSTRUCT(tuple))->bindrelid;
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return storage;
}
