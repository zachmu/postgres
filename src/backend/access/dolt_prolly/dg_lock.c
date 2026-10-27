/*-------------------------------------------------------------------------
 *
 * dg_lock.c
 *	  Row locks and unique-key reservations for prolly relations.
 *
 * Each prolly row version is shadowed by a lock record in pg_dolt_lock, keyed
 * by (storage, branch, handle). The record behaves as the row's heap tuple
 * would: locking the row locks the record, updating the row updates the record
 * to name the new version's handle, and deleting the row deletes the record.
 * Heap code therefore decides every concurrency outcome: lock strengths and
 * multixacts, waits and deadlock detection, NOWAIT and SKIP LOCKED, the
 * TM_Result an update or delete sees, and following and locking update chains.
 * A transaction that waited on a version learns its successor's handle from
 * the record that replaced it.
 *
 * A record is created the first time its row is locked, updated or deleted on
 * its branch. It is inserted frozen, so it is visible to everyone at once and
 * survives its creator's abort: a record is only an anchor, and the lock its
 * creator then takes on it is ordinary. A short heavyweight lock keeps two
 * backends from creating the same record.
 *
 * Version chains live only in per-branch records, so creating a branch copies
 * nothing.
 *
 * src/backend/access/dolt_prolly/dg_lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_lock.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

typedef struct Records
{
	Relation	heap;
	Relation	key;			/* by first handle */
	Relation	handle;			/* by current handle */
} Records;

static Records
records_open(void)
{
	Records		r;

	r.heap = table_open(DoltLockRelationId, RowExclusiveLock);
	r.key = index_open(DoltLockKeyIndexId, RowExclusiveLock);
	r.handle = index_open(DoltLockHandleIndexId, RowExclusiveLock);
	return r;
}

static void
records_close(Records *r)
{
	index_close(r->handle, RowExclusiveLock);
	index_close(r->key, RowExclusiveLock);
	table_close(r->heap, RowExclusiveLock);
}

/* The record whose current handle is |id|; false if there is none. */
static bool
find_record(Records *r, RelFileLocator *locator, const char *branch, int64 id, ItemPointer tid)
{
	ScanKeyData keys[4];
	IndexScanDesc scan;
	TupleTableSlot *slot = table_slot_create(r->heap, NULL);
	bool		found = false;

	/* Index column positions: (space, file, handle, branch). */
	ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(locator->spcOid));
	ScanKeyInit(&keys[1], 2, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(locator->relNumber));
	ScanKeyInit(&keys[2], 3, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(id));
	ScanKeyInit(&keys[3], 4, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = index_beginscan(r->heap, r->handle, SnapshotAny, NULL, 4, 0);
	index_rescan(scan, keys, 4, NULL, 0);
	while (index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;

		/* Index entries outlive the handle a record had when they were made. */
		if (DatumGetInt64(slot_getattr(slot, Anum_pg_dolt_lock_lockhandle, &isnull)) == id)
		{
			*tid = slot->tts_tid;
			found = true;
			break;
		}
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	return found;
}

/* Add index entries for a new record tuple. */
static void
index_record(Records *r, HeapTuple tuple, bool key_too)
{
	Relation	indexes[2] = {r->handle, r->key};
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(r->heap), &TTSOpsHeapTuple);
	EState	   *estate = CreateExecutorState();

	ExecStoreHeapTuple(tuple, slot, false);
	for (int i = 0; i < (key_too ? 2 : 1); i++)
	{
		IndexInfo  *info = BuildIndexInfo(indexes[i]);
		Datum		values[INDEX_MAX_KEYS];
		bool		nulls[INDEX_MAX_KEYS];

		FormIndexDatum(info, slot, estate, values, nulls);
		index_insert(indexes[i], values, nulls, &tuple->t_self, r->heap,
					 indexes[i]->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, info);
	}
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
}

/*
 * The record for handle |id|, created frozen if no record has ever named it.
 * A record that named it and was since updated or deleted still names it, so
 * a handle's record is never created twice.
 */
static void
record_for(Records *r, RelFileLocator *locator, const char *branch, int64 id, ItemPointer tid)
{
	uint32		tag;
	Datum		values[Natts_pg_dolt_lock];
	bool		nulls[Natts_pg_dolt_lock] = {false};
	HeapTuple	tuple;

	if (find_record(r, locator, branch, id, tid))
		return;
	tag = hash_bytes_extended((const unsigned char *) branch, strlen(branch),
							  ((uint64) locator->relNumber << 32) ^ (uint64) id);
	LockDatabaseObject(DoltLockRelationId, tag, 0, ExclusiveLock);
	if (!find_record(r, locator, branch, id, tid))
	{
		values[Anum_pg_dolt_lock_lockspace - 1] = ObjectIdGetDatum(locator->spcOid);
		values[Anum_pg_dolt_lock_lockfile - 1] = ObjectIdGetDatum(locator->relNumber);
		values[Anum_pg_dolt_lock_lockroot - 1] = Int64GetDatum(id);
		values[Anum_pg_dolt_lock_lockhandle - 1] = Int64GetDatum(id);
		values[Anum_pg_dolt_lock_lockbranch - 1] = CStringGetTextDatum(branch);
		tuple = heap_form_tuple(RelationGetDescr(r->heap), values, nulls);
		heap_insert(r->heap, tuple, GetCurrentCommandId(true), HEAP_INSERT_FROZEN, NULL);
		index_record(r, tuple, true);
		*tid = tuple->t_self;
		heap_freetuple(tuple);
	}
	UnlockDatabaseObject(DoltLockRelationId, tag, 0, ExclusiveLock);
}

/* The current handle of the record version at |tid|. */
static int64
record_handle(Records *r, ItemPointer tid)
{
	HeapTupleData tuple;
	Buffer		buffer;
	int64		id;

	tuple.t_self = *tid;
	if (!heap_fetch(r->heap, SnapshotAny, &tuple, &buffer, false))
		elog(ERROR, "prolly row lock record is missing");
	id = ((Form_pg_dolt_lock) GETSTRUCT(&tuple))->lockhandle;
	ReleaseBuffer(buffer);
	return id;
}

static pg_noreturn void
lock_unavailable(Relation rel)
{
	ereport(ERROR,
			(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
			 errmsg("could not obtain lock on row in relation \"%s\"", RelationGetRelationName(rel))));
}

/*
 * The handle of the version that replaced row version |id| of |rel| on
 * |branch|, or 0 if it was deleted or never replaced.
 */
int64
dg_record_successor(Relation rel, const char *branch, int64 id)
{
	Records		r = records_open();
	ItemPointerData tid;
	HeapTupleData tuple;
	Buffer		buffer;
	int64		next = 0;

	if (find_record(&r, &rel->rd_locator, branch, id, &tid))
	{
		tuple.t_self = tid;
		if (!heap_fetch(r.heap, SnapshotAny, &tuple, &buffer, false))
			elog(ERROR, "prolly row lock record is missing");
		if (!ItemPointerEquals(&tuple.t_self, &tuple.t_data->t_ctid))
		{
			ItemPointerData ctid = tuple.t_data->t_ctid;

			ReleaseBuffer(buffer);
			next = record_handle(&r, &ctid);
		}
		else
			ReleaseBuffer(buffer);
	}
	records_close(&r);
	return next;
}

/* Whether |xid| belongs to another transaction that is still running. */
static bool
running_elsewhere(TransactionId xid)
{
	return !TransactionIdIsCurrentTransactionId(xid) && TransactionIdIsInProgress(xid);
}

/*
 * A running transaction that a dirty-snapshot reader of record |id| must wait
 * for, as heap's dirty snapshot reports one in xmax: the transaction updating
 * or deleting the row, or, if |lockers|, any other holding a lock on the
 * record, as an inserter holds its unique-key reservation.
 */
TransactionId
dg_record_xwait(Relation rel, const char *branch, int64 id, bool lockers)
{
	Records		r = records_open();
	HeapTupleData tuple;
	Buffer		buffer;
	TransactionId xmax;
	uint16		infomask;
	TransactionId xwait = InvalidTransactionId;

	if (!find_record(&r, &rel->rd_locator, branch, id, &tuple.t_self))
	{
		records_close(&r);
		return InvalidTransactionId;
	}
	if (!heap_fetch(r.heap, SnapshotAny, &tuple, &buffer, false))
		elog(ERROR, "prolly row lock record is missing");
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	xmax = HeapTupleHeaderGetRawXmax(tuple.t_data);
	infomask = tuple.t_data->t_infomask;
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	if (infomask & HEAP_XMAX_INVALID)
		xmax = InvalidTransactionId;
	if (TransactionIdIsValid(xmax) && (infomask & HEAP_XMAX_IS_MULTI))
	{
		MultiXactMember *members;
		int			n = GetMultiXactIdMembers(xmax, &members, false, HEAP_XMAX_IS_LOCKED_ONLY(infomask));

		for (int i = 0; i < n && !TransactionIdIsValid(xwait); i++)
			if ((lockers || ISUPDATE_from_mxstatus(members[i].status)) && running_elsewhere(members[i].xid))
				xwait = members[i].xid;
		if (n > 0)
			pfree(members);
	}
	else if (TransactionIdIsValid(xmax) && (lockers || !HEAP_XMAX_IS_LOCKED_ONLY(infomask)) &&
			 running_elsewhere(xmax))
		xwait = xmax;
	records_close(&r);
	return xwait;
}

/* Report a replaced record's successor as a TID in the row's relation. */
static void
successor(Records *r, TM_Result result, TM_FailureData *tmfd, int64 id, int64 *next)
{
	*next = 0;
	if (result == TM_Ok || result == TM_WouldBlock)
		return;					/* no conflicting version; ids may be key hashes */
	if (result == TM_Updated && ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
		return;					/* the executor reports the move */
	if (result == TM_Updated)
		*next = record_handle(r, &tmfd->ctid);
	dg_tid((uint64) (*next ? *next : id), &tmfd->ctid);
}

/*
 * Lock row version |id| of |rel| on |branch| in |mode|, as heap_lock_tuple
 * locks a tuple for command |cid|; |follow| also locks later versions, as
 * SELECT FOR UPDATE does. Under NOWAIT a conflict raises the usual error. On
 * TM_Updated, *next is the successor's handle and tmfd->ctid its TID.
 *
 * A record's versions mirror its row's, made by the same commands, so heap
 * decides, as for the row itself, how this transaction's own earlier changes
 * affect the command.
 */
TM_Result
dg_record_lock(Relation rel, const char *branch, int64 id, CommandId cid, LockTupleMode mode, LockWaitPolicy wait,
			   bool follow, TM_FailureData *tmfd, int64 *next)
{
	Records		r = records_open();
	HeapTupleData tuple;
	Buffer		buffer = InvalidBuffer;
	TM_Result	result;

	record_for(&r, &rel->rd_locator, branch, id, &tuple.t_self);
	result = heap_lock_tuple(r.heap, &tuple, cid, mode,
							 wait == LockWaitError ? LockWaitSkip : wait, follow, &buffer, tmfd);
	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);
	if (result == TM_WouldBlock && wait == LockWaitError)
		lock_unavailable(rel);
	successor(&r, result, tmfd, id, next);
	records_close(&r);
	return result;
}

/*
 * Update row version |id| to |next_id| in command |cid|, as heap_update
 * updates a tuple. A key change first takes the stronger lock heap would. On
 * TM_Updated, *next is the concurrent successor's handle.
 */
TM_Result
dg_record_update(Relation rel, const char *branch, int64 id, int64 next_id, CommandId cid, bool key_change, bool wait,
				 TM_FailureData *tmfd, int64 *next)
{
	Records		r = records_open();
	ItemPointerData tid;
	TM_Result	result = TM_Ok;
	HeapTupleData probe;
	Buffer		buffer = InvalidBuffer;

	record_for(&r, &rel->rd_locator, branch, id, &tid);
	if (key_change)
	{
		probe.t_self = tid;
		result = heap_lock_tuple(r.heap, &probe, cid, LockTupleExclusive,
								 wait ? LockWaitBlock : LockWaitError, false, &buffer, tmfd);
		if (BufferIsValid(buffer))
			ReleaseBuffer(buffer);
	}
	if (result == TM_Ok)
	{
		HeapTuple	old,
					tuple;
		Datum		values[Natts_pg_dolt_lock] = {0};
		bool		nulls[Natts_pg_dolt_lock] = {false};
		bool		replace[Natts_pg_dolt_lock] = {false};
		LockTupleMode mode;
		TU_UpdateIndexes indexes;

		probe.t_self = tid;
		if (!heap_fetch(r.heap, SnapshotAny, &probe, &buffer, false))
			elog(ERROR, "prolly row lock record is missing");
		old = heap_copytuple(&probe);
		ReleaseBuffer(buffer);
		values[Anum_pg_dolt_lock_lockhandle - 1] = Int64GetDatum(next_id);
		replace[Anum_pg_dolt_lock_lockhandle - 1] = true;
		tuple = heap_modify_tuple(old, RelationGetDescr(r.heap), values, nulls, replace);
		result = heap_update(r.heap, &tid, tuple, cid, InvalidSnapshot, wait,
							 tmfd, &mode, &indexes);
		if (result == TM_Ok)
			index_record(&r, tuple, false);
		heap_freetuple(tuple);
		heap_freetuple(old);
	}
	successor(&r, result, tmfd, id, next);
	records_close(&r);
	return result;
}

/*
 * Delete row version |id| in command |cid|, as heap_delete deletes a tuple.
 * |changing_part| marks a row moving to another partition, so waiters learn
 * of the move.
 */
TM_Result
dg_record_delete(Relation rel, const char *branch, int64 id, CommandId cid, bool wait, bool changing_part,
				 TM_FailureData *tmfd, int64 *next)
{
	Records		r = records_open();
	ItemPointerData tid;
	TM_Result	result;

	record_for(&r, &rel->rd_locator, branch, id, &tid);
	result = heap_delete(r.heap, &tid, cid, InvalidSnapshot, wait, tmfd, changing_part);
	successor(&r, result, tmfd, id, next);
	records_close(&r);
	return result;
}
