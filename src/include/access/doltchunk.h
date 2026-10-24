/*-------------------------------------------------------------------------
 *
 * doltchunk.h
 *	  Native access to the pg_dolt_chunk store and the per-backend node cache.
 *
 * Callers supply the SHA-256 hash of each chunk; the store trusts it. Chunks
 * are immutable, so cached copies never need invalidation.
 *
 * src/include/access/doltchunk.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTCHUNK_H
#define DOLTCHUNK_H

#include "storage/lockdefs.h"

#define DOLT_CHUNK_HASH_SIZE 32

typedef struct DoltChunkStore DoltChunkStore;

typedef struct DoltChunkCacheStats
{
	uint64		hits;
	uint64		misses;
	uint64		evictions;
	uint64		entries;
	uint64		bytes;
} DoltChunkCacheStats;

extern PGDLLIMPORT int dolt_chunk_cache_size;

/* Open with AccessShareLock to read, or RowExclusiveLock to also write. */
extern DoltChunkStore *dolt_chunk_open(LOCKMODE lockmode);
extern void dolt_chunk_close(DoltChunkStore *store);

/*
 * Returns the chunk's bytes, or NULL if no row has this hash. The result stays
 * valid until the next dolt_chunk_get or dolt_chunk_put call in this backend.
 */
extern const uint8 *dolt_chunk_get(DoltChunkStore *store, const uint8 *hash, Size *length);

/* True if a committed, own or in-progress-then-committed row has this hash. */
extern bool dolt_chunk_exists(DoltChunkStore *store, const uint8 *hash);

/* Inserts the chunk unless present; returns true if this call inserted it. */
extern bool dolt_chunk_put(DoltChunkStore *store, const uint8 *hash, const uint8 *data, Size length);

/* Largest chunk that fits inline in one heap tuple. */
extern Size dolt_chunk_max_size(void);

extern void dolt_chunk_cache_stats(DoltChunkCacheStats *stats);
extern void dolt_chunk_cache_reset(void);

#endif							/* DOLTCHUNK_H */
