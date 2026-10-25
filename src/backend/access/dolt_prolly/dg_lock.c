/*-------------------------------------------------------------------------
 *
 * dg_lock.c
 *	  Row locks and unique-key reservations for prolly relations.
 *
 * A prolly row has no tuple header, so its locks live on a lock record in
 * pg_dolt_lock, keyed by (storage, branch, handle). The record is an ordinary
 * heap tuple, and locking it with heap_lock_tuple provides the four lock
 * strengths, multixacts, waits, deadlock detection, NOWAIT and SKIP LOCKED.
 *
 * A record is created the first time its row is locked, updated or deleted on
 * its branch. It is inserted frozen, so it is visible to everyone at once and
 * survives the creator's abort; a record is only an anchor, and the lock the
 * creator then takes on it is ordinary. Otherwise the uncommitted record would
 * lock the row exclusively until its creator ended, even against compatible
 * lockers. A short-lived heavyweight lock keeps two backends from creating the
 * same record.
 *
 * Updating a row gives the new version a new handle. The writer records that
 * successor (the heir) on the old version's lock record by updating it, so a
 * transaction that waited on the old version can follow the chain under Read
 * Committed. Heirs matter only to transactions running concurrently on the
 * same branch, so branch creation never copies them.
 *
 * src/backend/access/dolt_prolly/dg_lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_lock.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "common/hashfn.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

/* A record version's state, judged without waiting or setting hint bits. */
typedef enum RecordState
{
	RECORD_DEAD,				/* aborted insert, or replaced by an update */
	RECORD_LIVE,				/* the current version */
	RECORD_INSERTING,			/* inserted by a running transaction */
} RecordState;

static RecordState
record_state(HeapTuple tuple, Buffer buffer, TransactionId *inserter)
{
	HeapTupleHeader header = tuple->t_data;
	TransactionId xmin;
	RecordState state = RECORD_LIVE;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	xmin = HeapTupleHeaderGetRawXmin(header);
	if (HeapTupleHeaderXminInvalid(header))
		state = RECORD_DEAD;
	else if (HeapTupleHeaderXminCommitted(header) || TransactionIdIsCurrentTransactionId(xmin))
		state = RECORD_LIVE;
	else if (TransactionIdIsInProgress(xmin))
	{
		state = RECORD_INSERTING;
		*inserter = xmin;
	}
	else if (!TransactionIdDidCommit(xmin))
		state = RECORD_DEAD;

	/* A version replaced by a committed or own update is not current. */
	if (state == RECORD_LIVE && !(header->t_infomask & HEAP_XMAX_INVALID) &&
		!HEAP_XMAX_IS_LOCKED_ONLY(header->t_infomask) &&
		ItemPointerIsValid(&header->t_ctid) && !ItemPointerEquals(&header->t_ctid, &tuple->t_self))
	{
		TransactionId xmax = HeapTupleHeaderGetUpdateXid(header);

		if (TransactionIdIsCurrentTransactionId(xmax) || TransactionIdDidCommit(xmax))
			state = RECORD_DEAD;
	}
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	return state;
}

typedef struct Found
{
	ItemPointerData tid;		/* the live version, if valid */
	TransactionId inserter;		/* else a running inserter to wait for */
	int64		heir;
} Found;

static Found
find_record(Relation catalog, Relation index, RelFileLocator *locator, const char *branch, int64 id)
{
	ScanKeyData keys[4];
	IndexScanDesc scan;
	TupleTableSlot *slot = table_slot_create(catalog, NULL);
	Found		found = {0};

	ItemPointerSetInvalid(&found.tid);
	/* Index column positions: (space, file, handle, branch). */
	ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(locator->spcOid));
	ScanKeyInit(&keys[1], 2, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(locator->relNumber));
	ScanKeyInit(&keys[2], 3, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(id));
	ScanKeyInit(&keys[3], 4, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = index_beginscan(catalog, index, SnapshotAny, NULL, 4, 0);
	index_rescan(scan, keys, 4, NULL, 0);
	while (index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
		TransactionId inserter = InvalidTransactionId;
		RecordState state = record_state(bslot->base.tuple, bslot->buffer, &inserter);

		if (state == RECORD_LIVE)
		{
			bool		isnull;

			found.tid = slot->tts_tid;
			found.heir = DatumGetInt64(slot_getattr(slot, Anum_pg_dolt_lock_lockheir, &isnull));
			break;
		}
		if (state == RECORD_INSERTING)
			found.inserter = inserter;
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	return found;
}

static pg_noreturn void
lock_unavailable(Relation rel)
{
	ereport(ERROR,
			(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
			 errmsg("could not obtain lock on row in relation \"%s\"", RelationGetRelationName(rel))));
}

/*
 * Create the record for |id| unless one exists. The tuple is frozen: visible
 * to every transaction now, whatever becomes of this one.
 */
static void
create_record(Relation catalog, Relation index, RelFileLocator *locator, const char *branch, int64 id)
{
	uint32		tag = hash_bytes_extended((const unsigned char *) branch, strlen(branch),
										  ((uint64) locator->relNumber << 32) ^ (uint64) id);
	Datum		values[Natts_pg_dolt_lock];
	bool		nulls[Natts_pg_dolt_lock] = {false};
	HeapTuple	tuple;
	IndexInfo  *info;
	Datum		index_values[INDEX_MAX_KEYS];
	bool		index_nulls[INDEX_MAX_KEYS];
	TupleTableSlot *slot;
	EState	   *estate;
	Found		found;

	LockDatabaseObject(DoltLockRelationId, tag, 0, ExclusiveLock);
	found = find_record(catalog, index, locator, branch, id);
	if (ItemPointerIsValid(&found.tid) || TransactionIdIsValid(found.inserter))
	{
		UnlockDatabaseObject(DoltLockRelationId, tag, 0, ExclusiveLock);
		return;
	}
	values[Anum_pg_dolt_lock_lockspace - 1] = ObjectIdGetDatum(locator->spcOid);
	values[Anum_pg_dolt_lock_lockfile - 1] = ObjectIdGetDatum(locator->relNumber);
	values[Anum_pg_dolt_lock_lockhandle - 1] = Int64GetDatum(id);
	values[Anum_pg_dolt_lock_lockheir - 1] = Int64GetDatum(0);
	values[Anum_pg_dolt_lock_lockbranch - 1] = CStringGetTextDatum(branch);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	heap_insert(catalog, tuple, GetCurrentCommandId(true), HEAP_INSERT_FROZEN, NULL);
	info = BuildIndexInfo(index);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(catalog), &TTSOpsHeapTuple);
	estate = CreateExecutorState();
	ExecStoreHeapTuple(tuple, slot, false);
	FormIndexDatum(info, slot, estate, index_values, index_nulls);
	index_insert(index, index_values, index_nulls, &tuple->t_self, catalog, UNIQUE_CHECK_NO, false, info);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	heap_freetuple(tuple);
	UnlockDatabaseObject(DoltLockRelationId, tag, 0, ExclusiveLock);
}

/*
 * Lock the record for row (or key) |id| of |rel| on |branch| in |mode|,
 * creating it if needed. Returns TM_Ok, or TM_WouldBlock under SKIP LOCKED;
 * raises the usual error under NOWAIT. *heir receives the successor handle if
 * a committed or own update replaced the row, else zero.
 *
 */
TM_Result
dg_lock_record(Relation rel, const char *branch, int64 id, LockTupleMode mode,
			   LockWaitPolicy wait, int64 *heir)
{
	Relation	catalog = table_open(DoltLockRelationId, RowExclusiveLock);
	Relation	index = index_open(DoltLockKeyIndexId, RowExclusiveLock);
	TM_Result	result = TM_Ok;

	*heir = 0;
	for (;;)
	{
		Found		found = find_record(catalog, index, &rel->rd_locator, branch, id);

		CHECK_FOR_INTERRUPTS();
		if (ItemPointerIsValid(&found.tid))
		{
			HeapTupleData tuple;
			Buffer		buffer = InvalidBuffer;
			TM_FailureData failure;
			TM_Result	locked;

			tuple.t_self = found.tid;
			locked = heap_lock_tuple(catalog, &tuple, GetCurrentCommandId(false), mode,
									 wait == LockWaitError ? LockWaitSkip : wait, false,
									 &buffer, &failure);
			if (BufferIsValid(buffer))
				ReleaseBuffer(buffer);
			if (locked == TM_Ok || locked == TM_SelfModified || locked == TM_Invisible)
			{
				/* Own records are held already; heir reflects the latest. */
				if (locked == TM_Ok)
					found = find_record(catalog, index, &rel->rd_locator, branch, id);
				*heir = found.heir;
				break;
			}
			if (locked == TM_WouldBlock)
			{
				if (wait == LockWaitError)
					lock_unavailable(rel);
				result = TM_WouldBlock;
				break;
			}

			/* Replaced by an update that committed while we waited: retry. */
			continue;
		}
		if (TransactionIdIsValid(found.inserter))
		{
			if (wait == LockWaitSkip)
			{
				result = TM_WouldBlock;
				break;
			}
			if (wait == LockWaitError)
				lock_unavailable(rel);
			XactLockTableWait(found.inserter, NULL, NULL, XLTW_None);
			continue;
		}

		create_record(catalog, index, &rel->rd_locator, branch, id);
	}
	index_close(index, RowExclusiveLock);
	table_close(catalog, RowExclusiveLock);
	return result;
}

/*
 * Record |heir| as the successor of row |id|, whose record the caller holds.
 * Does nothing if the record already names it.
 */
void
dg_lock_set_heir(Relation rel, const char *branch, int64 id, int64 heir)
{
	Relation	catalog = table_open(DoltLockRelationId, RowExclusiveLock);
	Relation	index = index_open(DoltLockKeyIndexId, RowExclusiveLock);
	Found		found = find_record(catalog, index, &rel->rd_locator, branch, id);
	HeapTuple	old,
				tuple;
	Datum		values[Natts_pg_dolt_lock] = {0};
	bool		nulls[Natts_pg_dolt_lock] = {false};
	bool		replace[Natts_pg_dolt_lock] = {false};
	HeapTupleData probe;
	Buffer		buffer;
	TM_FailureData failure;
	LockTupleMode mode;
	TU_UpdateIndexes indexes;
	CommandId	cid = GetCurrentCommandId(true);
	TM_Result	result;

	if (!ItemPointerIsValid(&found.tid))
		elog(ERROR, "prolly row lock record is missing");
	if (found.heir == heir)
	{
		index_close(index, RowExclusiveLock);
		table_close(catalog, RowExclusiveLock);
		return;
	}
	probe.t_self = found.tid;
	if (!heap_fetch(catalog, SnapshotAny, &probe, &buffer, false))
		elog(ERROR, "prolly row lock record is missing");
	old = heap_copytuple(&probe);
	ReleaseBuffer(buffer);
	values[Anum_pg_dolt_lock_lockheir - 1] = Int64GetDatum(heir);
	replace[Anum_pg_dolt_lock_lockheir - 1] = true;
	tuple = heap_modify_tuple(old, RelationGetDescr(catalog), values, nulls, replace);

	result = heap_update(catalog, &found.tid, tuple, cid, InvalidSnapshot, true, &failure, &mode, &indexes);
	if (result != TM_Ok)
		elog(ERROR, "could not record prolly row successor: %d", result);
	if (indexes != TU_None)
	{
		IndexInfo  *info = BuildIndexInfo(index);
		TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(catalog), &TTSOpsHeapTuple);
		EState	   *estate = CreateExecutorState();
		Datum		index_values[INDEX_MAX_KEYS];
		bool		index_nulls[INDEX_MAX_KEYS];

		ExecStoreHeapTuple(tuple, slot, false);
		FormIndexDatum(info, slot, estate, index_values, index_nulls);
		index_insert(index, index_values, index_nulls, &tuple->t_self, catalog, UNIQUE_CHECK_YES, false, info);
		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
	}
	heap_freetuple(tuple);
	heap_freetuple(old);
	index_close(index, RowExclusiveLock);
	table_close(catalog, RowExclusiveLock);
}
