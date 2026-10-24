CREATE EXTENSION test_dolt_chunk;

--
-- Interface contract, against the built-in heap store and then a test-only,
-- non-transactional in-memory store.
--
CREATE FUNCTION error_of(query text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE query;
  RETURN NULL;
EXCEPTION WHEN OTHERS THEN
  RETURN SQLERRM;
END
$$;
\getenv abs_srcdir PG_ABS_SRCDIR
\set contract :abs_srcdir '/sql/contract.inc'
\i :contract
SELECT test_chunk_use_store('memory') AS previous_store;
\i :contract
-- Rollback semantics are not part of the contract: this store keeps puts.
BEGIN;
SELECT test_chunk_put(sha256('memory rollback'), 'memory rollback');
ROLLBACK;
SELECT test_chunk_exists(sha256('memory rollback')) AS kept_after_rollback;
SELECT test_chunk_use_store('heap') AS previous_store;
SELECT test_chunk_use_store('bogus');

--
-- Heap implementation details.
--

-- Storage is inline and uncompressed, keyed by a unique hash index.
SELECT attname, attstorage FROM pg_attribute
  WHERE attrelid = 'pg_dolt_chunk'::regclass AND attnum > 0 ORDER BY attnum;
SELECT reltoastrelid = 0 AS no_toast FROM pg_class WHERE oid = 'pg_dolt_chunk'::regclass;
SELECT indisunique, indisprimary FROM pg_index WHERE indexrelid = 'pg_dolt_chunk_hash_index'::regclass;
SELECT name, setting, unit FROM pg_settings
  WHERE name IN ('dolt_chunk_cache_size', 'dolt_chunk_verify_hashes') ORDER BY name;

-- One row per hash, however often it was put.
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = sha256('contract alpha');

-- A chunk longer than one heap tuple spans parts, each inline and uncompressed.
SELECT test_chunk_put(sha256(x), x) FROM (SELECT decode(repeat('cd', 20000), 'hex') AS x) s;
SELECT chunkpart, chunksize, length(chunkdata), pg_column_compression(chunkdata) IS NULL AS uncompressed
  FROM pg_dolt_chunk WHERE chunkhash = sha256(decode(repeat('cd', 20000), 'hex')) ORDER BY chunkpart;
SELECT test_chunk_get(sha256(x)) = x AS multipart_roundtrip
  FROM (SELECT decode(repeat('cd', 20000), 'hex') AS x) s;
SELECT test_chunk_delete(sha256(decode(repeat('cd', 20000), 'hex'))) AS multipart_deleted;
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = sha256(decode(repeat('cd', 20000), 'hex'));

-- Puts are transactional here: own writes are visible, rollback removes them.
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

-- A deleted chunk can be put again.
SELECT test_chunk_put(sha256('contract delta'), 'contract delta') AS reput_after_delete;
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = sha256('contract delta');

-- Existence probes do not set hint bits on rows they examine. With data
-- checksums, a hint on a page's first change after a checkpoint logs a
-- full-page image. pageinspect reads the row without touching hints.
CREATE EXTENSION pageinspect;
CREATE FUNCTION chunk_xmin_hinted(hash bytea) RETURNS bool LANGUAGE sql AS $$
  SELECT (t_infomask & 256) <> 0
    FROM generate_series(0, pg_relation_size('pg_dolt_chunk') / 8192 - 1) AS b,
         heap_page_items(get_raw_page('pg_dolt_chunk', b::int)) AS i
   WHERE t_data IS NOT NULL AND position(hash IN t_data) > 0
$$;
SELECT test_chunk_put(sha256('hintless'), 'hintless');
SELECT test_chunk_exists(sha256('hintless')) AS exists;
SELECT test_chunk_put(sha256('hintless'), 'hintless') AS reput;
SELECT chunk_xmin_hinted(sha256('hintless')) AS hinted_after_probes;
-- Control: an ordinary MVCC read of the row sets the hint.
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = sha256('hintless');
SELECT chunk_xmin_hinted(sha256('hintless')) AS hinted_after_select;
DROP FUNCTION chunk_xmin_hinted(bytea);
DROP EXTENSION pageinspect;

-- Read-only transactions cannot write.
BEGIN READ ONLY;
SELECT test_chunk_put(sha256('delta'), 'delta');
ROLLBACK;

-- The cache serves repeated reads and stays within its budget.
SET dolt_chunk_cache_size = '64kB';
SELECT test_chunk_cache_reset();
SELECT test_chunk_get(sha256('contract alpha')) = 'contract alpha' AS first_read;
SELECT test_chunk_get(sha256('contract alpha')) = 'contract alpha' AS second_read;
SELECT hits, misses FROM test_chunk_cache_stats();
SELECT count(*) FILTER (WHERE test_chunk_put(sha256(x), x)) AS inserted
  FROM (SELECT decode(repeat(lpad(to_hex(g), 2, '0'), 8000), 'hex') AS x
        FROM generate_series(1, 20) g) s;
SELECT evictions > 0 AS evicted, bytes <= 65536 AS within_budget, entries < 20 AS bounded,
       pinned = 0 AS unpinned
  FROM test_chunk_cache_stats();
SELECT count(*) FILTER (WHERE test_chunk_get(sha256(x)) = x) AS all_readable
  FROM (SELECT decode(repeat(lpad(to_hex(g), 2, '0'), 8000), 'hex') AS x
        FROM generate_series(1, 20) g) s;

-- With caching disabled, reads still work and nothing is retained.
SET dolt_chunk_cache_size = 0;
SELECT test_chunk_cache_reset();
SELECT test_chunk_get(sha256('contract alpha')) = 'contract alpha' AS uncached_read;
SELECT hits, misses, entries, bytes, pinned FROM test_chunk_cache_stats();
RESET dolt_chunk_cache_size;

-- The catalog is private.
CREATE ROLE regress_chunk_reader;
SET ROLE regress_chunk_reader;
SELECT count(*) FROM pg_dolt_chunk;
RESET ROLE;
DROP ROLE regress_chunk_reader;
