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
 * A chunk longer than one heap tuple holds is stored as consecutive parts of
 * DG_CHUNK_PART bytes. Part 0 decides whether a chunk exists, and its inserter
 * owns the remaining parts. Because a part's bytes follow from the chunk and
 * its number, every copy of a part is identical, including copies left behind
 * by aborted inserts.
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
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* The largest part: a heap tuple less its header, part, size and hash. */
#define DG_CHUNK_PART \
	(MaxHeapTupleSize - (MAXALIGN(SizeofHeapTupleHeader) + 2 * sizeof(int32) + \
						 INTALIGN(1 + DOLT_CHUNK_HASH_SIZE) + VARHDRSZ))

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
	/* Parts are addressed by an int32 and a chunk must fit one allocation. */
	return MaxAllocSize - VARHDRSZ;
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

/* Position the scan on the rows carrying |hash|, or only its part |part|. */
static void
probe(HeapChunkState *state, const uint8 *hash, int part)
{
	ScanKeyData keys[2];

	if (!state->scan)
		state->scan = index_beginscan(state->heap, state->index, SnapshotAny, NULL, 2, 0);
	memcpy(VARDATA(state->key), hash, DOLT_CHUNK_HASH_SIZE);
	ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, F_BYTEAEQ, PointerGetDatum(state->key));
	if (part < 0)
		ScanKeyInit(&keys[1], 2, BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
	else
		ScanKeyInit(&keys[1], 2, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(part));
	index_rescan(state->scan, keys, 2, NULL, 0);
}

static int32
slot_int(TupleTableSlot *slot, AttrNumber attribute)
{
	bool		isnull;

	return DatumGetInt32(slot_getattr(slot, attribute, &isnull));
}

static struct varlena *
slot_data(TupleTableSlot *slot)
{
	bool		isnull;
	struct varlena *value = (struct varlena *)
		DatumGetPointer(slot_getattr(slot, Anum_pg_dolt_chunk_chunkdata, &isnull));

	if (isnull || (VARATT_IS_EXTENDED(value) && !VARATT_IS_SHORT(value)))
		elog(ERROR, "dolt chunk is not stored inline");
	return value;
}

static void
heap_get(void *arg, int n, const uint8 *hashes, DoltChunkFoundFn found, void *found_arg)
{
	HeapChunkState *state = arg;

	for (int i = 0; i < n; i++)
	{
		uint8	   *assembled = NULL;
		int32		size = -1,
					next = 0;

		probe(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, -1);

		/*
		 * Parts arrive in order. Copies of a part are identical, so the first
		 * copy of each part is used and later ones are skipped.
		 */
		while (index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		{
			int32		part = slot_int(state->slot, Anum_pg_dolt_chunk_chunkpart);
			struct varlena *value;
			Size		length;

			if (part < next)
				continue;
			if (part > next)
				elog(ERROR, "dolt chunk is missing part %d", next);
			value = slot_data(state->slot);
			length = VARSIZE_ANY_EXHDR(value);
			if (part == 0)
			{
				size = slot_int(state->slot, Anum_pg_dolt_chunk_chunksize);
				if (length == (Size) size)
				{
					found(found_arg, i, (const uint8 *) VARDATA_ANY(value), length);
					next = -1;
					break;
				}
				assembled = palloc(size);
			}
			if ((Size) part * DG_CHUNK_PART + length > (Size) size)
				elog(ERROR, "dolt chunk part %d is too long", part);
			memcpy(assembled + (Size) part * DG_CHUNK_PART, VARDATA_ANY(value), length);
			next++;
			if ((Size) part * DG_CHUNK_PART + length == (Size) size)
			{
				found(found_arg, i, assembled, size);
				next = -1;
				break;
			}
		}
		ExecClearTuple(state->slot);
		if (next > 0)
			elog(ERROR, "dolt chunk is missing part %d", next);
		if (assembled)
			pfree(assembled);
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
find_live(HeapChunkState *state, const uint8 *hash, int part)
{
	for (;;)
	{
		TransactionId wait = InvalidTransactionId;
		uint32		token = 0;
		ItemPointerData tid;
		bool		live = false;

		probe(state, hash, part);
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
		found[i] = find_live(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, 0);
		ExecClearTuple(state->slot);
	}
}

/* Form part |part| of a chunk; the caller frees the tuple and its values. */
static HeapTuple
form_part(HeapChunkState *state, bytea *hashvalue, const uint8 *data, Size length, int32 part,
		  Datum *index_values)
{
	Datum		values[Natts_pg_dolt_chunk];
	bool		nulls[Natts_pg_dolt_chunk] = {false};
	Size		offset = (Size) part * DG_CHUNK_PART;
	Size		piece = Min(DG_CHUNK_PART, length - offset);
	bytea	   *datavalue = palloc(VARHDRSZ + piece);
	HeapTuple	tuple;

	SET_VARSIZE(datavalue, VARHDRSZ + piece);
	memcpy(VARDATA(datavalue), data + offset, piece);
	values[Anum_pg_dolt_chunk_chunkpart - 1] = Int32GetDatum(part);
	values[Anum_pg_dolt_chunk_chunksize - 1] = Int32GetDatum((int32) length);
	values[Anum_pg_dolt_chunk_chunkhash - 1] = PointerGetDatum(hashvalue);
	values[Anum_pg_dolt_chunk_chunkdata - 1] = PointerGetDatum(datavalue);
	tuple = heap_form_tuple(RelationGetDescr(state->heap), values, nulls);
	pfree(datavalue);
	index_values[0] = PointerGetDatum(hashvalue);
	index_values[1] = Int32GetDatum(part);
	return tuple;
}

/* Insert one chunk unless a live row has its hash; true if inserted. */
static bool
put_one(HeapChunkState *state, const uint8 *hash, const uint8 *data, Size length)
{
	bytea	   *hashvalue = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	Datum		index_values[2];
	bool		index_nulls[2] = {false};
	int32		parts = length ? (int32) ((length + DG_CHUNK_PART - 1) / DG_CHUNK_PART) : 1;
	bool		inserted = false;

	SET_VARSIZE(hashvalue, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	memcpy(VARDATA(hashvalue), hash, DOLT_CHUNK_HASH_SIZE);
	while (!find_live(state, hash, 0))
	{
		HeapTuple	tuple;
		uint32		token;

		/*
		 * Speculative insertion of part 0: concurrent probes wait on our
		 * token, and a unique conflict found by the index cancels our row and
		 * retries.
		 */
		tuple = form_part(state, hashvalue, data, length, 0, index_values);
		token = SpeculativeInsertionLockAcquire(GetCurrentTransactionId());
		HeapTupleHeaderSetSpeculativeToken(tuple->t_data, token);
		heap_insert(state->heap, tuple, GetCurrentCommandId(true), HEAP_INSERT_SPECULATIVE, NULL);
		inserted = index_insert(state->index, index_values, index_nulls, &tuple->t_self,
								state->heap, UNIQUE_CHECK_PARTIAL, false, state->index_info);
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

	/* Owning part 0 makes this transaction the only writer of the rest. */
	for (int32 part = 1; inserted && part < parts; part++)
	{
		HeapTuple	tuple = form_part(state, hashvalue, data, length, part, index_values);

		heap_insert(state->heap, tuple, GetCurrentCommandId(true), 0, NULL);
		index_insert(state->index, index_values, index_nulls, &tuple->t_self, state->heap,
					 UNIQUE_CHECK_YES, false, state->index_info);
		heap_freetuple(tuple);
	}
	pfree(hashvalue);
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
		bytea	   *hash;

		if (slot_int(slot, Anum_pg_dolt_chunk_chunkpart) != 0)
			continue;
		hash = DatumGetByteaPP(slot_getattr(slot, Anum_pg_dolt_chunk_chunkhash, &isnull));
		if (!fn(fn_arg, (const uint8 *) VARDATA_ANY(hash), slot_int(slot, Anum_pg_dolt_chunk_chunksize)))
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
		const uint8 *hash = hashes + (Size) i * DOLT_CHUNK_HASH_SIZE;
		ItemPointerData *tids;
		int			ntids = 0,
					maxtids = 4;

		if (!find_live(state, hash, 0))
			continue;
		ExecClearTuple(state->slot);

		/* Delete every live part; dead copies are left to VACUUM. */
		tids = palloc(maxtids * sizeof(ItemPointerData));
		probe(state, hash, -1);
		while (index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		{
			TransactionId wait;
			uint32		token;

			if (row_status(state, &wait, &token) != ROW_LIVE)
				continue;
			if (ntids == maxtids)
			{
				maxtids *= 2;
				tids = repalloc(tids, maxtids * sizeof(ItemPointerData));
			}
			tids[ntids++] = state->slot->tts_tid;
		}
		ExecClearTuple(state->slot);
		for (int t = 0; t < ntids; t++)
			simple_heap_delete(state->heap, &tids[t]);
		pfree(tids);
		removed++;
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
