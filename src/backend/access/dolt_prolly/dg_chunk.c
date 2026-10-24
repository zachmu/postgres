/*-------------------------------------------------------------------------
 *
 * dg_chunk.c
 *	  Native access to pg_dolt_chunk and the per-backend node cache.
 *
 * Reads probe the unique hash index with SnapshotAny: a chunk's bytes are fixed
 * by its hash, so any row carrying that hash has the right content, whoever
 * inserted it. Writers dedupe with a dirty-snapshot probe and insert with the
 * speculative insertion protocol INSERT ... ON CONFLICT DO NOTHING uses, so a
 * concurrent insert of the same chunk waits for its inserter instead of raising
 * a unique violation. Inserts are transactional; aborted rows become dead
 * tuples for VACUUM.
 *
 * The node cache maps hashes to copies of chunk bytes, least recently used
 * entries evicted first above dolt_chunk_cache_size. Chunks are immutable, so
 * entries never need invalidation.
 *
 * src/backend/access/dolt_prolly/dg_chunk.c
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
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_dolt_chunk.h"
#include "executor/tuptable.h"
#include "lib/ilist.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

int			dolt_chunk_cache_size = 256 * 1024; /* kB */

struct DoltChunkStore
{
	Relation	heap;
	Relation	index;
	LOCKMODE	lockmode;
	IndexInfo  *index_info;		/* built on first write */
	TupleTableSlot *slot;
	IndexScanDesc any_scan;		/* readers: content is fixed by the hash */
	IndexScanDesc dirty_scan;	/* writers: sees in-progress inserts */
	SnapshotData dirty;
	bytea	   *key;			/* scan key datum: the probed hash */
	uint8	   *scratch;		/* result buffer when a chunk is not cached */
	Size		scratch_size;
};

typedef struct CacheEntry
{
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	uint8	   *data;
	Size		length;
	dlist_node	lru;
} CacheEntry;

static MemoryContext cache_context;
static HTAB *cache;
static dlist_head cache_lru = DLIST_STATIC_INIT(cache_lru);
static Size cache_bytes;
static DoltChunkCacheStats cache_stats;

/* Bytes an entry is charged against dolt_chunk_cache_size. */
static Size
entry_cost(Size length)
{
	return length + sizeof(CacheEntry) + 64;
}

static void
cache_init(void)
{
	HASHCTL		ctl = {0};

	if (cache)
		return;
	cache_context = AllocSetContextCreate(TopMemoryContext, "dolt chunk cache", ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = DOLT_CHUNK_HASH_SIZE;
	ctl.entrysize = sizeof(CacheEntry);
	ctl.hcxt = cache_context;
	cache = hash_create("dolt chunk cache", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static CacheEntry *
cache_lookup(const uint8 *hash)
{
	CacheEntry *entry;

	cache_init();
	entry = hash_search(cache, hash, HASH_FIND, NULL);
	if (entry)
		dlist_move_head(&cache_lru, &entry->lru);
	return entry;
}

static void
cache_evict(CacheEntry *entry)
{
	dlist_delete(&entry->lru);
	cache_bytes -= entry_cost(entry->length);
	pfree(entry->data);
	hash_search(cache, entry->hash, HASH_REMOVE, NULL);
	cache_stats.evictions++;
}

/*
 * Copy a chunk into the cache, evicting least recently used entries to make
 * room. Returns the cached copy, or NULL if the chunk exceeds the whole budget.
 */
static const uint8 *
cache_insert(const uint8 *hash, const uint8 *data, Size length)
{
	Size		limit = (Size) dolt_chunk_cache_size * 1024;
	CacheEntry *entry;
	bool		found;

	cache_init();
	if (entry_cost(length) > limit)
		return NULL;
	while (cache_bytes + entry_cost(length) > limit && !dlist_is_empty(&cache_lru))
		cache_evict(dlist_container(CacheEntry, lru, dlist_tail_node(&cache_lru)));

	entry = hash_search(cache, hash, HASH_ENTER, &found);
	if (found)
	{
		dlist_move_head(&cache_lru, &entry->lru);
		return entry->data;
	}
	entry->data = MemoryContextAlloc(cache_context, Max(length, 1));
	memcpy(entry->data, data, length);
	entry->length = length;
	dlist_push_head(&cache_lru, &entry->lru);
	cache_bytes += entry_cost(length);
	return entry->data;
}

void
dolt_chunk_cache_stats(DoltChunkCacheStats *stats)
{
	*stats = cache_stats;
	stats->entries = cache ? hash_get_num_entries(cache) : 0;
	stats->bytes = cache_bytes;
}

void
dolt_chunk_cache_reset(void)
{
	if (cache)
	{
		MemoryContextDelete(cache_context);
		cache = NULL;
		cache_context = NULL;
	}
	dlist_init(&cache_lru);
	cache_bytes = 0;
	memset(&cache_stats, 0, sizeof(cache_stats));
}

Size
dolt_chunk_max_size(void)
{
	/*
	 * A heap tuple holds the header, the hash (short varlena header, padded
	 * to chunkdata's int alignment) and the data with a 4-byte header.
	 */
	return MaxHeapTupleSize -
		(MAXALIGN(SizeofHeapTupleHeader) + INTALIGN(1 + DOLT_CHUNK_HASH_SIZE) + VARHDRSZ);
}

DoltChunkStore *
dolt_chunk_open(LOCKMODE lockmode)
{
	DoltChunkStore *store = palloc0(sizeof(DoltChunkStore));

	Assert(lockmode == AccessShareLock || lockmode == RowExclusiveLock);
	store->lockmode = lockmode;
	store->heap = table_open(DoltChunkRelationId, lockmode);
	store->index = index_open(DoltChunkHashIndexId, lockmode);
	store->slot = table_slot_create(store->heap, NULL);
	store->key = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	SET_VARSIZE(store->key, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	return store;
}

void
dolt_chunk_close(DoltChunkStore *store)
{
	if (store->any_scan)
		index_endscan(store->any_scan);
	if (store->dirty_scan)
		index_endscan(store->dirty_scan);
	ExecDropSingleTupleTableSlot(store->slot);
	index_close(store->index, store->lockmode);
	table_close(store->heap, store->lockmode);
	if (store->scratch)
		pfree(store->scratch);
	pfree(store->key);
	pfree(store);
}

/* Position a scan on the rows carrying |hash|. */
static void
probe(DoltChunkStore *store, IndexScanDesc scan, const uint8 *hash)
{
	ScanKeyData key;

	memcpy(VARDATA(store->key), hash, DOLT_CHUNK_HASH_SIZE);
	ScanKeyInit(&key, Anum_pg_dolt_chunk_chunkhash, BTEqualStrategyNumber, F_BYTEAEQ,
				PointerGetDatum(store->key));
	index_rescan(scan, &key, 1, NULL, 0);
}

const uint8 *
dolt_chunk_get(DoltChunkStore *store, const uint8 *hash, Size *length)
{
	CacheEntry *entry = cache_lookup(hash);
	const uint8 *result = NULL;

	if (entry)
	{
		cache_stats.hits++;
		*length = entry->length;
		return entry->data;
	}
	cache_stats.misses++;
	if (!store->any_scan)
		store->any_scan = index_beginscan(store->heap, store->index, SnapshotAny, NULL, 1, 0);
	probe(store, store->any_scan, hash);
	if (index_getnext_slot(store->any_scan, ForwardScanDirection, store->slot))
	{
		bool		isnull;
		struct varlena *value = (struct varlena *)
			DatumGetPointer(slot_getattr(store->slot, Anum_pg_dolt_chunk_chunkdata, &isnull));

		if (isnull || (VARATT_IS_EXTENDED(value) && !VARATT_IS_SHORT(value)))
			elog(ERROR, "dolt chunk is not stored inline");
		*length = VARSIZE_ANY_EXHDR(value);
		result = cache_insert(hash, (const uint8 *) VARDATA_ANY(value), *length);
		if (!result)
		{
			if (store->scratch_size < *length)
			{
				if (store->scratch)
					pfree(store->scratch);
				store->scratch = MemoryContextAlloc(GetMemoryChunkContext(store), *length);
				store->scratch_size = *length;
			}
			memcpy(store->scratch, VARDATA_ANY(value), *length);
			result = store->scratch;
		}
	}
	ExecClearTuple(store->slot);
	return result;
}

bool
dolt_chunk_exists(DoltChunkStore *store, const uint8 *hash)
{
	for (;;)
	{
		TransactionId wait = InvalidTransactionId;
		uint32		token = 0;
		ItemPointerData tid;
		bool		found = false;

		InitDirtySnapshot(store->dirty);
		if (!store->dirty_scan)
			store->dirty_scan = index_beginscan(store->heap, store->index, &store->dirty, NULL, 1, 0);
		probe(store, store->dirty_scan, hash);
		if (index_getnext_slot(store->dirty_scan, ForwardScanDirection, store->slot))
		{
			found = true;
			tid = store->slot->tts_tid;

			/* An in-progress inserter or deleter decides whether the row lives. */
			wait = TransactionIdIsValid(store->dirty.xmin) ? store->dirty.xmin : store->dirty.xmax;
			token = store->dirty.speculativeToken;
		}
		ExecClearTuple(store->slot);
		if (!TransactionIdIsValid(wait))
			return found;

		/* Never wait while the scan holds index buffer pins. */
		index_endscan(store->dirty_scan);
		store->dirty_scan = NULL;
		if (token)
			SpeculativeInsertionWait(wait, token);
		else
			XactLockTableWait(wait, store->heap, &tid, XLTW_InsertIndex);
	}
}

bool
dolt_chunk_put(DoltChunkStore *store, const uint8 *hash, const uint8 *data, Size length)
{
	Datum		values[Natts_pg_dolt_chunk];
	bool		nulls[Natts_pg_dolt_chunk] = {false};
	bytea	   *hashvalue;
	bytea	   *datavalue;

	if (store->lockmode != RowExclusiveLock)
		elog(ERROR, "dolt chunk store opened read-only");
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot write dolt chunks in a read-only transaction")));
	if (length > dolt_chunk_max_size())
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("dolt chunk of %zu bytes exceeds the inline limit of %zu bytes",
						length, dolt_chunk_max_size())));
	if (!store->index_info)
		store->index_info = BuildIndexInfo(store->index);

	hashvalue = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	SET_VARSIZE(hashvalue, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	memcpy(VARDATA(hashvalue), hash, DOLT_CHUNK_HASH_SIZE);
	datavalue = palloc(VARHDRSZ + length);
	SET_VARSIZE(datavalue, VARHDRSZ + length);
	memcpy(VARDATA(datavalue), data, length);
	values[Anum_pg_dolt_chunk_chunkhash - 1] = PointerGetDatum(hashvalue);
	values[Anum_pg_dolt_chunk_chunkdata - 1] = PointerGetDatum(datavalue);

	for (;;)
	{
		HeapTuple	tuple;
		uint32		token;
		bool		inserted;

		if (dolt_chunk_exists(store, hash))
		{
			pfree(hashvalue);
			pfree(datavalue);
			return false;
		}

		/*
		 * Speculative insertion: concurrent probes wait on our token, and a
		 * unique conflict found by the index cancels our row and retries.
		 */
		tuple = heap_form_tuple(RelationGetDescr(store->heap), values, nulls);
		token = SpeculativeInsertionLockAcquire(GetCurrentTransactionId());
		HeapTupleHeaderSetSpeculativeToken(tuple->t_data, token);
		heap_insert(store->heap, tuple, GetCurrentCommandId(true), HEAP_INSERT_SPECULATIVE, NULL);
		inserted = index_insert(store->index, &values[Anum_pg_dolt_chunk_chunkhash - 1], nulls,
								&tuple->t_self, store->heap, UNIQUE_CHECK_PARTIAL, false,
								store->index_info);
		if (inserted)
			heap_finish_speculative(store->heap, &tuple->t_self);
		else
			heap_abort_speculative(store->heap, &tuple->t_self);
		SpeculativeInsertionLockRelease(GetCurrentTransactionId());
		heap_freetuple(tuple);

		if (inserted)
		{
			cache_insert(hash, data, length);
			pfree(hashvalue);
			pfree(datavalue);
			return true;
		}
	}
}
