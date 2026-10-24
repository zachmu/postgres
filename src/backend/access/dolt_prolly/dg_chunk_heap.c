/*-------------------------------------------------------------------------
 *
 * dg_chunk_heap.c
 *	  The pg_dolt_chunk heap catalog as a chunk store implementation.
 *
 * Reads probe the unique hash index with SnapshotAny: a chunk's bytes are fixed
 * by its hash, so any row carrying that hash has the right content.
 *
 * Existence probes also scan with SnapshotAny, then decide whether a row lives
 * from its inserting and deleting transactions' status, read without setting
 * hint bits. Setting hints on first probe would dirty each page and, with data
 * checksums, log a full-page image per page. A row whose inserter or deleter is
 * still running is waited on.
 *
 * Puts follow the speculative insertion protocol INSERT ... ON CONFLICT DO
 * NOTHING uses, so a concurrent insert of the same chunk waits for its inserter
 * instead of raising a unique violation. Puts are transactional here; aborted
 * rows become dead tuples for VACUUM. Callers must not depend on that.
 *
 * src/backend/access/dolt_prolly/dg_chunk_heap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_dolt_chunk.h"
#include "executor/tuptable.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

typedef struct HeapChunkState
{
	Relation	heap;
	Relation	index;
	LOCKMODE	lockmode;
	IndexInfo  *index_info;		/* built on first write */
	TupleTableSlot *slot;
	IndexScanDesc scan;			/* SnapshotAny probes by hash */
	bytea	   *key;			/* scan key datum: the probed hash */
} HeapChunkState;

static Size
heap_max_size(void)
{
	/*
	 * A heap tuple holds the header, the hash (short varlena header, padded
	 * to chunkdata's int alignment) and the data with a 4-byte header.
	 */
	return MaxHeapTupleSize -
		(MAXALIGN(SizeofHeapTupleHeader) + INTALIGN(1 + DOLT_CHUNK_HASH_SIZE) + VARHDRSZ);
}

static void *
heap_open_store(DoltChunkIntent intent)
{
	HeapChunkState *state = palloc0(sizeof(HeapChunkState));

	state->lockmode = intent == DOLT_CHUNK_WRITE ? RowExclusiveLock : AccessShareLock;
	state->heap = table_open(DoltChunkRelationId, state->lockmode);
	state->index = index_open(DoltChunkHashIndexId, state->lockmode);
	state->slot = table_slot_create(state->heap, NULL);
	state->key = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	SET_VARSIZE(state->key, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	return state;
}

static void
heap_close_store(void *arg)
{
	HeapChunkState *state = arg;

	if (state->scan)
		index_endscan(state->scan);
	ExecDropSingleTupleTableSlot(state->slot);
	index_close(state->index, state->lockmode);
	table_close(state->heap, state->lockmode);
	pfree(state->key);
	pfree(state);
}

/* Position the scan on the rows carrying |hash|. */
static void
probe(HeapChunkState *state, const uint8 *hash)
{
	ScanKeyData key;

	if (!state->scan)
		state->scan = index_beginscan(state->heap, state->index, SnapshotAny, NULL, 1, 0);
	memcpy(VARDATA(state->key), hash, DOLT_CHUNK_HASH_SIZE);
	ScanKeyInit(&key, Anum_pg_dolt_chunk_chunkhash, BTEqualStrategyNumber, F_BYTEAEQ,
				PointerGetDatum(state->key));
	index_rescan(state->scan, &key, 1, NULL, 0);
}

static void
heap_get(void *arg, int n, const uint8 *hashes, DoltChunkFoundFn found, void *found_arg)
{
	HeapChunkState *state = arg;

	for (int i = 0; i < n; i++)
	{
		probe(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE);
		if (index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		{
			bool		isnull;
			struct varlena *value = (struct varlena *)
				DatumGetPointer(slot_getattr(state->slot, Anum_pg_dolt_chunk_chunkdata, &isnull));

			if (isnull || (VARATT_IS_EXTENDED(value) && !VARATT_IS_SHORT(value)))
				elog(ERROR, "dolt chunk is not stored inline");
			found(found_arg, i, (const uint8 *) VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
		}
		ExecClearTuple(state->slot);
	}
}

typedef enum RowStatus
{
	ROW_LIVE,
	ROW_DEAD,
	ROW_WAIT,
} RowStatus;

/*
 * Classify the row in the slot from its transactions' status, without setting
 * hint bits. Sets *wait (and *token for a speculative insert) for ROW_WAIT.
 */
static RowStatus
row_status(HeapChunkState *state, TransactionId *wait, uint32 *token)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) state->slot;
	HeapTupleHeader tuple;
	RowStatus	status = ROW_LIVE;

	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	tuple = bslot->base.tuple->t_data;
	*token = 0;
	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		TransactionId xmin = HeapTupleHeaderGetRawXmin(tuple);

		if (HeapTupleHeaderXminInvalid(tuple))
			status = ROW_DEAD;
		else if (TransactionIdIsCurrentTransactionId(xmin))
			status = ROW_LIVE;
		else if (TransactionIdIsInProgress(xmin))
		{
			status = ROW_WAIT;
			*wait = xmin;
			if (HeapTupleHeaderIsSpeculative(tuple))
				*token = HeapTupleHeaderGetSpeculativeToken(tuple);
		}
		else if (!TransactionIdDidCommit(xmin))
			status = ROW_DEAD;
	}
	if (status == ROW_LIVE && !(tuple->t_infomask & HEAP_XMAX_INVALID) &&
		!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		TransactionId xmax;

		/* Chunk rows are only inserted and deleted, never updated or locked. */
		if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			elog(ERROR, "unexpected multixact on a dolt chunk row");
		xmax = HeapTupleHeaderGetRawXmax(tuple);
		if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
			status = ROW_DEAD;
		else if (TransactionIdIsCurrentTransactionId(xmax))
			status = ROW_DEAD;
		else if (TransactionIdIsInProgress(xmax))
		{
			status = ROW_WAIT;
			*wait = xmax;
		}
		else if (TransactionIdDidCommit(xmax))
			status = ROW_DEAD;
	}
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);
	return status;
}

/*
 * Find a live row for |hash|, waiting out in-progress inserters and deleters.
 * Leaves the row in the slot if found.
 */
static bool
find_live(HeapChunkState *state, const uint8 *hash)
{
	for (;;)
	{
		TransactionId wait = InvalidTransactionId;
		uint32		token = 0;
		ItemPointerData tid;
		bool		live = false;

		probe(state, hash);
		while (index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		{
			RowStatus	status = row_status(state, &wait, &token);

			if (status == ROW_LIVE)
			{
				live = true;
				break;
			}
			if (status == ROW_WAIT)
			{
				tid = state->slot->tts_tid;
				break;
			}
		}
		if (!TransactionIdIsValid(wait))
		{
			if (!live)
				ExecClearTuple(state->slot);
			return live;
		}

		/* Never wait while the scan holds index buffer pins. */
		ExecClearTuple(state->slot);
		index_endscan(state->scan);
		state->scan = NULL;
		if (token)
			SpeculativeInsertionWait(wait, token);
		else
			XactLockTableWait(wait, state->heap, &tid, XLTW_InsertIndex);
	}
}

static void
heap_exists(void *arg, int n, const uint8 *hashes, bool *found)
{
	HeapChunkState *state = arg;

	for (int i = 0; i < n; i++)
	{
		found[i] = find_live(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE);
		ExecClearTuple(state->slot);
	}
}

/* Insert one chunk unless a live row has its hash; true if inserted. */
static bool
put_one(HeapChunkState *state, const uint8 *hash, const uint8 *data, Size length)
{
	Datum		values[Natts_pg_dolt_chunk];
	bool		nulls[Natts_pg_dolt_chunk] = {false};
	bytea	   *hashvalue = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	bytea	   *datavalue = palloc(VARHDRSZ + length);
	bool		inserted = false;

	SET_VARSIZE(hashvalue, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	memcpy(VARDATA(hashvalue), hash, DOLT_CHUNK_HASH_SIZE);
	SET_VARSIZE(datavalue, VARHDRSZ + length);
	memcpy(VARDATA(datavalue), data, length);
	values[Anum_pg_dolt_chunk_chunkhash - 1] = PointerGetDatum(hashvalue);
	values[Anum_pg_dolt_chunk_chunkdata - 1] = PointerGetDatum(datavalue);

	while (!find_live(state, hash))
	{
		HeapTuple	tuple;
		uint32		token;

		/*
		 * Speculative insertion: concurrent probes wait on our token, and a
		 * unique conflict found by the index cancels our row and retries.
		 */
		tuple = heap_form_tuple(RelationGetDescr(state->heap), values, nulls);
		token = SpeculativeInsertionLockAcquire(GetCurrentTransactionId());
		HeapTupleHeaderSetSpeculativeToken(tuple->t_data, token);
		heap_insert(state->heap, tuple, GetCurrentCommandId(true), HEAP_INSERT_SPECULATIVE, NULL);
		inserted = index_insert(state->index, &values[Anum_pg_dolt_chunk_chunkhash - 1], nulls,
								&tuple->t_self, state->heap, UNIQUE_CHECK_PARTIAL, false,
								state->index_info);
		if (inserted)
			heap_finish_speculative(state->heap, &tuple->t_self);
		else
			heap_abort_speculative(state->heap, &tuple->t_self);
		SpeculativeInsertionLockRelease(GetCurrentTransactionId());
		heap_freetuple(tuple);
		if (inserted)
			break;
	}
	ExecClearTuple(state->slot);
	pfree(hashvalue);
	pfree(datavalue);
	return inserted;
}

static const uint8 *sort_hashes;

static int
compare_hashes(const void *a, const void *b)
{
	return memcmp(sort_hashes + (Size) *(const int *) a * DOLT_CHUNK_HASH_SIZE,
				  sort_hashes + (Size) *(const int *) b * DOLT_CHUNK_HASH_SIZE,
				  DOLT_CHUNK_HASH_SIZE);
}

static int
heap_put(void *arg, int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths)
{
	HeapChunkState *state = arg;
	int		   *order = palloc(Max(n, 1) * sizeof(int));
	int			stored = 0;

	if (!state->index_info)
		state->index_info = BuildIndexInfo(state->index);

	/* Insert in hash order, so consecutive index insertions share leaf pages. */
	for (int i = 0; i < n; i++)
		order[i] = i;
	sort_hashes = hashes;
	qsort(order, n, sizeof(int), compare_hashes);
	for (int i = 0; i < n; i++)
	{
		int			j = order[i];

		if (put_one(state, hashes + (Size) j * DOLT_CHUNK_HASH_SIZE, data[j], lengths[j]))
			stored++;
	}
	pfree(order);
	return stored;
}

static void
heap_scan(void *arg, DoltChunkScanFn fn, void *fn_arg)
{
	HeapChunkState *state = arg;
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = table_beginscan(state->heap, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(state->heap, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		bytea	   *hash = DatumGetByteaPP(slot_getattr(slot, Anum_pg_dolt_chunk_chunkhash, &isnull));
		bytea	   *data = DatumGetByteaPP(slot_getattr(slot, Anum_pg_dolt_chunk_chunkdata, &isnull));

		if (!fn(fn_arg, (const uint8 *) VARDATA_ANY(hash), VARSIZE_ANY_EXHDR(data)))
			break;
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
}

static int
heap_remove(void *arg, int n, const uint8 *hashes)
{
	HeapChunkState *state = arg;
	int			removed = 0;

	for (int i = 0; i < n; i++)
	{
		if (find_live(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE))
		{
			ItemPointerData tid = state->slot->tts_tid;

			ExecClearTuple(state->slot);
			simple_heap_delete(state->heap, &tid);
			removed++;
		}
	}
	return removed;
}

static void
heap_stats(DoltChunkStoreStats *stats)
{
	Relation	heap = table_open(DoltChunkRelationId, AccessShareLock);
	Relation	index = index_open(DoltChunkHashIndexId, AccessShareLock);

	stats->chunks = Max(heap->rd_rel->reltuples, 0);
	stats->bytes = (uint64) (RelationGetNumberOfBlocks(heap) + RelationGetNumberOfBlocks(index)) * BLCKSZ;
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
}

static const DoltChunkStoreRoutine heap_routine = {
	.name = "heap",
	.max_size = heap_max_size,
	.open = heap_open_store,
	.close = heap_close_store,
	.get = heap_get,
	.exists = heap_exists,
	.put = heap_put,
	.scan = heap_scan,
	.remove = heap_remove,
	.stats = heap_stats,
};

const DoltChunkStoreRoutine *
dolt_chunk_heap_routine(void)
{
	return &heap_routine;
}
