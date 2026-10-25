-- Contract tests: behavior every chunk store implementation must provide.
-- Included once per implementation by test_dolt_chunk.sql, which defines
-- error_of(): expected errors are returned as text, since psql would prefix
-- them with this file's absolute path.

-- Puts are idempotent, and reads return the bytes put under a hash.
SELECT test_chunk_put(sha256('contract alpha'), 'contract alpha') AS first_put;
SELECT test_chunk_put(sha256('contract alpha'), 'contract alpha') AS second_put;
SELECT test_chunk_get(sha256('contract alpha')) = 'contract alpha' AS roundtrip;
SELECT test_chunk_get(sha256('contract missing')) IS NULL AS missing_get;
SELECT test_chunk_exists(sha256('contract alpha')) AS present,
       test_chunk_exists(sha256('contract missing')) AS absent;
SELECT error_of($$SELECT test_chunk_get('\x00')$$);

-- Batches: a put may contain duplicates of each other and of stored chunks.
SELECT test_chunk_put_many(
         ARRAY[sha256('contract beta'), sha256('contract gamma'), sha256('contract beta'), sha256('contract alpha')],
         ARRAY['contract beta', 'contract gamma', 'contract beta', 'contract alpha']::bytea[]) AS stored;
SELECT test_chunk_get_many(ARRAY[sha256('contract gamma'), sha256('contract missing'), sha256('contract beta')])
       = ARRAY['contract gamma', NULL, 'contract beta']::bytea[] AS batch_get;

-- Large chunks round-trip whole; the store reports a limit well above them.
SELECT test_chunk_max_size() >= 1048576 AS large_limit;
SELECT test_chunk_put(sha256(x), x) AS large_put,
       test_chunk_get(sha256(x)) = x AS large_roundtrip,
       test_chunk_exists(sha256(x)) AS large_exists
  FROM (SELECT decode(repeat(md5(g::text), 6250), 'hex') AS x FROM generate_series(1, 1) g) s;

-- Deleting removes a chunk from existence checks; deleting again finds nothing.
SELECT test_chunk_put(sha256('contract delta'), 'contract delta') AS put_delta;
SELECT test_chunk_delete(sha256('contract delta')) AS deleted;
SELECT test_chunk_exists(sha256('contract delta')) AS exists_after_delete;
SELECT test_chunk_delete(sha256('contract delta')) AS deleted_again;

-- Scans see stored chunks; statistics report something.
SELECT test_chunk_scan_count() >= 3 AS scanned;
SELECT chunks >= 0 AND bytes > 0 AS stats FROM test_chunk_store_stats();

-- Verification rejects content that does not match its hash.
SET dolt_chunk_verify_hashes = on;
SELECT test_chunk_put(sha256('contract epsilon'), 'contract epsilon') AS verified_put;
SELECT error_of($$SELECT test_chunk_put(sha256('contract zeta'), 'something else')$$);
RESET dolt_chunk_verify_hashes;

-- Pins keep a chunk's bytes valid while the cache evicts around them.
SET dolt_chunk_cache_size = '64kB';
SELECT test_chunk_cache_reset();
SELECT test_chunk_pin_survives_eviction(sha256('contract alpha'), 40) AS pin_survives;
SELECT pinned FROM test_chunk_cache_stats();
RESET dolt_chunk_cache_size;
