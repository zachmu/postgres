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
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalogview.h"
#include "catalog/indexing.h"
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

	ScanKeyInit(&key, Anum_pg_dolt_identity_identbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = systable_beginscan(catalog, DoltIdentityNameIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_dolt_identity form = (Form_pg_dolt_identity) GETSTRUCT(tuple);

		if (form->identkind == DOLT_IDENTITY_RELATION)
			CacheInvalidateRelcacheInDatabase(form->identoid);
		if (remove)
			CatalogTupleDelete(catalog, &tuple->t_self);
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
	CommandCounterIncrement();
}

/* Remove the identities of one object, when it is dropped from |branch|. */
void
dg_identity_delete(const char *branch, const char *name)
{
	Relation	catalog = table_open(DoltIdentityRelationId, RowExclusiveLock);
	char		kinds[] = {DOLT_IDENTITY_RELATION, DOLT_IDENTITY_ROWTYPE, DOLT_IDENTITY_ARRAYTYPE};

	for (int i = 0; i < lengthof(kinds); i++)
	{
		ScanKeyData keys[3];
		SysScanDesc scan;
		HeapTuple	tuple;

		name_keys(keys, branch, kinds[i], name);
		scan = systable_beginscan(catalog, DoltIdentityNameIndexId, true, NULL, 3, keys);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			if (kinds[i] == DOLT_IDENTITY_RELATION)
				CacheInvalidateRelcacheInDatabase(((Form_pg_dolt_identity) GETSTRUCT(tuple))->identoid);
			CatalogTupleDelete(catalog, &tuple->t_self);
		}
		systable_endscan(scan);
	}
	table_close(catalog, RowExclusiveLock);
	CommandCounterIncrement();
}
