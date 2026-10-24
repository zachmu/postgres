/*-------------------------------------------------------------------------
 *
 * pg_dolt_chunk.h
 *	  Database-wide store of prolly tree nodes, keyed by node hash.
 *
 * Every row is an immutable node addressed by the SHA-256 of its bytes. Data
 * stays inline and uncompressed: the catalog has no TOAST table and chunkdata
 * uses plain storage. Access goes through access/doltchunk.h, never SPI.
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
#ifdef CATALOG_VARLEN
	bytea		chunkhash BKI_FORCE_NOT_NULL;
	bytea		chunkdata BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_chunk;

typedef FormData_pg_dolt_chunk *Form_pg_dolt_chunk;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_chunk_hash_index, 9126, DoltChunkHashIndexId, pg_dolt_chunk, btree(chunkhash bytea_ops));

#endif							/* PG_DOLT_CHUNK_H */
