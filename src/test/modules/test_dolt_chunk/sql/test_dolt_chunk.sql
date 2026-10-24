CREATE EXTENSION test_dolt_chunk;

-- Storage is inline and uncompressed, keyed by a unique hash index.
SELECT attname, attstorage FROM pg_attribute
  WHERE attrelid = 'pg_dolt_chunk'::regclass AND attnum > 0 ORDER BY attnum;
SELECT reltoastrelid = 0 AS no_toast FROM pg_class WHERE oid = 'pg_dolt_chunk'::regclass;
SELECT indisunique, indisprimary FROM pg_index WHERE indexrelid = 'pg_dolt_chunk_hash_index'::regclass;
SELECT name, setting, unit FROM pg_settings WHERE name = 'dolt_chunk_cache_size';

-- Put, deduplicate, get and test existence.
SELECT test_chunk_put(sha256('alpha'), 'alpha');
SELECT test_chunk_put(sha256('alpha'), 'alpha');
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = sha256('alpha');
SELECT test_chunk_get(sha256('alpha')) = 'alpha' AS roundtrip;
SELECT test_chunk_get(sha256('missing')) IS NULL AS missing;
SELECT test_chunk_exists(sha256('alpha')) AS alpha, test_chunk_exists(sha256('missing')) AS missing;
SELECT test_chunk_get('\x00');

-- The inline limit is exact: the largest chunk fits uncompressed, one more byte fails.
SELECT test_chunk_max_size();
SELECT test_chunk_put(sha256(x), x)
  FROM (SELECT decode(repeat('ab', test_chunk_max_size()), 'hex') AS x) s;
SELECT length(chunkdata), pg_column_compression(chunkdata) IS NULL AS uncompressed
  FROM pg_dolt_chunk WHERE chunkhash = sha256(decode(repeat('ab', test_chunk_max_size()), 'hex'));
SELECT test_chunk_put(sha256(x), x)
  FROM (SELECT decode(repeat('ab', test_chunk_max_size() + 1), 'hex') AS x) s;

-- Inserts are transactional: own writes are visible, rollback removes them.
BEGIN;
SELECT test_chunk_put(sha256('beta'), 'beta');
SELECT test_chunk_exists(sha256('beta')) AS own_write;
SELECT test_chunk_put(sha256('beta'), 'beta') AS second_put;
ROLLBACK;
SELECT test_chunk_exists(sha256('beta')) AS after_rollback;
SELECT test_chunk_put(sha256('beta'), 'beta') AS reinserted;
BEGIN;
SAVEPOINT s;
SELECT test_chunk_put(sha256('gamma'), 'gamma');
ROLLBACK TO s;
SELECT test_chunk_exists(sha256('gamma')) AS after_savepoint_rollback;
COMMIT;
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash IN (sha256('alpha'), sha256('beta'), sha256('gamma'));

-- Read-only transactions cannot write.
BEGIN READ ONLY;
SELECT test_chunk_put(sha256('delta'), 'delta');
ROLLBACK;

-- The cache serves repeated reads and stays within its budget.
SET dolt_chunk_cache_size = '64kB';
SELECT test_chunk_cache_reset();
SELECT test_chunk_get(sha256('alpha')) = 'alpha' AS first_read;
SELECT test_chunk_get(sha256('alpha')) = 'alpha' AS second_read;
SELECT hits, misses FROM test_chunk_cache_stats();
SELECT count(*) FILTER (WHERE test_chunk_put(sha256(x), x)) AS inserted
  FROM (SELECT decode(repeat(lpad(to_hex(g), 2, '0'), 8000), 'hex') AS x
        FROM generate_series(1, 20) g) s;
SELECT evictions > 0 AS evicted, bytes <= 65536 AS within_budget, entries < 20 AS bounded
  FROM test_chunk_cache_stats();
SELECT count(*) FILTER (WHERE test_chunk_get(sha256(x)) = x) AS all_readable
  FROM (SELECT decode(repeat(lpad(to_hex(g), 2, '0'), 8000), 'hex') AS x
        FROM generate_series(1, 20) g) s;

-- With caching disabled, reads still work and nothing is retained.
SET dolt_chunk_cache_size = 0;
SELECT test_chunk_cache_reset();
SELECT test_chunk_get(sha256('alpha')) = 'alpha' AS uncached_read;
SELECT hits, misses, entries, bytes FROM test_chunk_cache_stats();
RESET dolt_chunk_cache_size;

-- The catalog is private.
CREATE ROLE regress_chunk_reader;
SET ROLE regress_chunk_reader;
SELECT count(*) FROM pg_dolt_chunk;
RESET ROLE;
DROP ROLE regress_chunk_reader;
