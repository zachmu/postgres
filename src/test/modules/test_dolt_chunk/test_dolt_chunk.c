/*--------------------------------------------------------------------------
 *
 * test_dolt_chunk.c
 *		SQL access to the dolt chunk store for tests and microbenchmarks.
 *
 * The benchmark functions loop inside one call so that their timings measure
 * the native store and cache, not SQL call overhead. Synthetic chunk i of a
 * seed has a pseudo-random hash and pseudo-random contents; the store trusts
 * caller-supplied hashes, so the hash need not be a digest of the contents.
 *
 * src/test/modules/test_dolt_chunk/test_dolt_chunk.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_chunk_put);
PG_FUNCTION_INFO_V1(test_chunk_get);
PG_FUNCTION_INFO_V1(test_chunk_exists);
PG_FUNCTION_INFO_V1(test_chunk_max_size);
PG_FUNCTION_INFO_V1(test_chunk_cache_stats);
PG_FUNCTION_INFO_V1(test_chunk_cache_reset);
PG_FUNCTION_INFO_V1(test_chunk_bench_put);
PG_FUNCTION_INFO_V1(test_chunk_bench_get);
PG_FUNCTION_INFO_V1(test_chunk_bench_get_spi);

static const uint8 *
hash_argument(bytea *hash)
{
	if (VARSIZE_ANY_EXHDR(hash) != DOLT_CHUNK_HASH_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("chunk hash must be %d bytes", DOLT_CHUNK_HASH_SIZE)));
	return (const uint8 *) VARDATA_ANY(hash);
}

Datum
test_chunk_put(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	bytea	   *data = PG_GETARG_BYTEA_PP(1);
	DoltChunkStore *store = dolt_chunk_open(RowExclusiveLock);
	bool		inserted;

	inserted = dolt_chunk_put(store, hash_argument(hash), (const uint8 *) VARDATA_ANY(data),
							  VARSIZE_ANY_EXHDR(data));
	dolt_chunk_close(store);
	PG_RETURN_BOOL(inserted);
}

Datum
test_chunk_get(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	DoltChunkStore *store = dolt_chunk_open(AccessShareLock);
	const uint8 *data;
	Size		length;
	bytea	   *result = NULL;

	data = dolt_chunk_get(store, hash_argument(hash), &length);
	if (data)
	{
		result = palloc(VARHDRSZ + length);
		SET_VARSIZE(result, VARHDRSZ + length);
		memcpy(VARDATA(result), data, length);
	}
	dolt_chunk_close(store);
	if (!result)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(result);
}

Datum
test_chunk_exists(PG_FUNCTION_ARGS)
{
	bytea	   *hash = PG_GETARG_BYTEA_PP(0);
	DoltChunkStore *store = dolt_chunk_open(AccessShareLock);
	bool		exists = dolt_chunk_exists(store, hash_argument(hash));

	dolt_chunk_close(store);
	PG_RETURN_BOOL(exists);
}

Datum
test_chunk_max_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) dolt_chunk_max_size());
}

Datum
test_chunk_cache_stats(PG_FUNCTION_ARGS)
{
	DoltChunkCacheStats stats;
	TupleDesc	desc;
	Datum		values[5];
	bool		nulls[5] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	dolt_chunk_cache_stats(&stats);
	values[0] = Int64GetDatum(stats.hits);
	values[1] = Int64GetDatum(stats.misses);
	values[2] = Int64GetDatum(stats.evictions);
	values[3] = Int64GetDatum(stats.entries);
	values[4] = Int64GetDatum(stats.bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

Datum
test_chunk_cache_reset(PG_FUNCTION_ARGS)
{
	dolt_chunk_cache_reset();
	PG_RETURN_VOID();
}

/* Size range for mixed-size benchmark chunks, as observed for prolly leaves. */
#define MIXED_MIN 512
#define MIXED_MAX 8000

static uint64
splitmix64(uint64 *state)
{
	uint64		z = (*state += UINT64CONST(0x9E3779B97F4A7C15));

	z = (z ^ (z >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return z ^ (z >> 31);
}

/* Synthetic chunk |i| of |seed|: its hash, and its contents if |data| is set. */
static void
synthetic_chunk(int32 seed, int64 i, uint8 *hash, uint8 *data, Size size)
{
	uint64		state = ((uint64) (uint32) seed << 40) ^ (uint64) i;
	Size		offset;

	for (offset = 0; offset < DOLT_CHUNK_HASH_SIZE; offset += 8)
	{
		uint64		word = splitmix64(&state);

		memcpy(hash + offset, &word, 8);
	}
	for (offset = 0; data && offset < size; offset += 8)
	{
		uint64		word = splitmix64(&state);

		memcpy(data + offset, &word, Min(8, size - offset));
	}
}

Datum
test_chunk_bench_put(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0);
	int64		first = PG_GETARG_INT64(1);
	int64		count = PG_GETARG_INT64(2);
	int32		size = PG_GETARG_INT32(3);
	DoltChunkStore *store = dolt_chunk_open(RowExclusiveLock);
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	uint8	   *data = palloc(size > 0 ? size : MIXED_MAX);
	instr_time	total;

	INSTR_TIME_SET_ZERO(total);
	for (int64 i = first; i < first + count; i++)
	{
		instr_time	start,
					end;
		Size		length = size;

		CHECK_FOR_INTERRUPTS();
		if (size <= 0)
		{
			/* Mixed sizes: uniform over the range observed for prolly leaves. */
			synthetic_chunk(seed, i, hash, NULL, 0);
			length = MIXED_MIN + ((hash[0] << 8) | hash[1]) % (MIXED_MAX - MIXED_MIN + 1);
		}
		synthetic_chunk(seed, i, hash, data, length);
		INSTR_TIME_SET_CURRENT(start);
		dolt_chunk_put(store, hash, data, length);
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(total, end, start);
	}
	dolt_chunk_close(store);
	PG_RETURN_FLOAT8(INSTR_TIME_GET_DOUBLE(total));
}

Datum
test_chunk_bench_get(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0);
	int64		nchunks = PG_GETARG_INT64(1);
	int64		count = PG_GETARG_INT64(2);
	uint64		rng = (uint64) PG_GETARG_INT32(3);
	DoltChunkStore *store = dolt_chunk_open(AccessShareLock);
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	instr_time	total;

	INSTR_TIME_SET_ZERO(total);
	for (int64 n = 0; n < count; n++)
	{
		instr_time	start,
					end;
		Size		length;
		const uint8 *data;

		CHECK_FOR_INTERRUPTS();
		synthetic_chunk(seed, (int64) (splitmix64(&rng) % (uint64) nchunks), hash, NULL, 0);
		INSTR_TIME_SET_CURRENT(start);
		data = dolt_chunk_get(store, hash, &length);
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(total, end, start);
		if (!data)
			elog(ERROR, "benchmark chunk missing");
	}
	dolt_chunk_close(store);
	PG_RETURN_FLOAT8(INSTR_TIME_GET_DOUBLE(total));
}

/* The same lookups through a prepared SPI query, for comparison. */
Datum
test_chunk_bench_get_spi(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0);
	int64		nchunks = PG_GETARG_INT64(1);
	int64		count = PG_GETARG_INT64(2);
	uint64		rng = (uint64) PG_GETARG_INT32(3);
	Oid			types[] = {BYTEAOID};
	bytea	   *key = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	SPIPlanPtr	plan;
	instr_time	total;

	SET_VARSIZE(key, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	plan = SPI_prepare("SELECT chunkdata FROM pg_catalog.pg_dolt_chunk WHERE chunkhash = $1", 1, types);
	if (!plan)
		elog(ERROR, "SPI_prepare failed");
	INSTR_TIME_SET_ZERO(total);
	for (int64 n = 0; n < count; n++)
	{
		instr_time	start,
					end;
		Datum		args[1] = {PointerGetDatum(key)};

		CHECK_FOR_INTERRUPTS();
		synthetic_chunk(seed, (int64) (splitmix64(&rng) % (uint64) nchunks),
						(uint8 *) VARDATA(key), NULL, 0);
		INSTR_TIME_SET_CURRENT(start);
		if (SPI_execute_plan(plan, args, NULL, true, 1) != SPI_OK_SELECT || SPI_processed != 1)
			elog(ERROR, "benchmark chunk missing");
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(total, end, start);
		SPI_freetuptable(SPI_tuptable);
	}
	SPI_finish();
	PG_RETURN_FLOAT8(INSTR_TIME_GET_DOUBLE(total));
}
