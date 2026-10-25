/*-------------------------------------------------------------------------
 *
 * pg_dolt_chunk.h
 *	  Database-wide store of prolly tree nodes, keyed by node hash.
 *
 * Every chunk is an immutable node addressed by the SHA-256 of its bytes. Data
 * stays inline and uncompressed: the catalog has no TOAST table and chunkdata
 * uses plain storage. A chunk larger than one heap tuple is split into parts
 * of a fixed size, so every copy of a given part has the same bytes. Access
 * goes through access/doltchunk.h, never SPI.
 *
 * Puts never wait for each other: a transaction that finds no committed copy
 * of a chunk inserts its own, tagged with its transaction ID. Concurrent puts
 * of one chunk may therefore leave duplicate copies, which are harmless.
 *
 * src/include/catalog/pg_dolt_chunk.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_CHUNK_H
#define PG_DOLT_CHUNK_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_chunk_d.h"

CATALOG(pg_dolt_chunk,9125,DoltChunkRelationId)
{
	int32		chunkpart BKI_FORCE_NOT_NULL;	/* 0, 1, ... */
	int32		chunksize BKI_FORCE_NOT_NULL;	/* the whole chunk's length */
	int64		chunkcopy BKI_FORCE_NOT_NULL;	/* inserting transaction */
#ifdef CATALOG_VARLEN
	bytea		chunkhash BKI_FORCE_NOT_NULL;
	bytea		chunkdata BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_chunk;

typedef FormData_pg_dolt_chunk *Form_pg_dolt_chunk;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_chunk_hash_index, 9126, DoltChunkHashIndexId, pg_dolt_chunk, btree(chunkhash bytea_ops, chunkpart int4_ops, chunkcopy int8_ops));

#endif							/* PG_DOLT_CHUNK_H */
