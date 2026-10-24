/* src/test/modules/test_dolt_chunk/test_dolt_chunk--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_dolt_chunk" to load this file. \quit

CREATE FUNCTION test_chunk_put(hash bytea, data bytea) RETURNS bool
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_get(hash bytea) RETURNS bytea
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_exists(hash bytea) RETURNS bool
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_max_size() RETURNS int4
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_cache_stats(OUT hits int8, OUT misses int8, OUT evictions int8,
									   OUT entries int8, OUT bytes int8)
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_cache_reset() RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Benchmarks over synthetic chunks: chunk i of a seed has a pseudo-random
-- hash and |size| pseudo-random bytes. Each returns seconds spent in the
-- store calls only, excluding data generation. A size of 0 draws each chunk's
-- size uniformly from 512 to 8000 bytes.
CREATE FUNCTION test_chunk_bench_put(seed int4, first int8, count int8, size int4) RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_bench_get(seed int4, nchunks int8, count int8, rngseed int4) RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_bench_get_spi(seed int4, nchunks int8, count int8, rngseed int4) RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
