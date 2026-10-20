# Experimental PostgreSQL prolly access methods

This is the phase 2 single-branch integration baseline. All runtime code is C.
It is opt-in, with a deliberately limited SQL surface and substantial performance
and lifecycle limitations. Ordinary tables and system catalogs continue to use
heap. It is not a production storage engine or a complete phase 3 implementation.

## Build and use

Build and install this PostgreSQL fork, including the new TableAmRoutine policy
callbacks. When changing that header, rebuild the server and extension together;
a full clean build avoids stale objects in builds without dependency tracking.
OpenSSL development headers and libcrypto are required by the prolly library.

```
make -C contrib/dolt_prolly USE_PGXS=1 PG_CONFIG=/path/to/installed/pg_config
make -C contrib/dolt_prolly USE_PGXS=1 PG_CONFIG=/path/to/installed/pg_config install
```

```
CREATE EXTENSION dolt_prolly;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE example(id integer PRIMARY KEY, value text COLLATE "C")
  USING dolt_prolly_table;
INSERT INTO example VALUES (1, 'hello');
CREATE INDEX example_value ON example USING btree(value);
SELECT * FROM example WHERE id = 1;
```

A table-AM callback routes SQL `btree` indexes on these tables to
`dolt_prolly_index`. Other index families fail explicitly. This does not change
btree indexes on heap tables. The extension supplies six default operator classes
for its AM, reusing PostgreSQL's built-in operators/comparison support functions.
Custom comparator functions, nonstandard strategy operators and non-C index
collations are rejected.

The extension owner's `dolt_prolly_root(regclass)` diagnostic reports the
canonical content hash of a table or prolly index. PUBLIC execution is revoked;
this bypasses normal SQL row selection and must not become a hash oracle for
unauthorized tables. The function is a diagnostic, not a branch/ref API.

## Architecture and decisions

### Canonical and runtime trees

A table content tree maps a SHA-256 digest of canonical row bytes to an
eight-byte occurrence count followed by the packed tuple. Internal routing keys
therefore stay fixed at 32 bytes; tuple payloads stay in leaves. A digest
collision between unequal rows is rejected. This multiset representation handles keyless duplicate rows and does not
change when rows are inserted in a different order. Primary-key enforcement is
performed by the unique ordered index, not by changing table content keys.

A separate runtime tree maps monotonically allocated 64-bit occurrence handles
to those 32-byte canonical row keys. Those handles are adapted into PostgreSQL ItemPointers;
they are not disk addresses. Updates allocate a new handle. The handle tree and
allocator counter are explicitly outside the canonical table root. Heap-style
XIDs, tuple padding, OIDs and lock bits are not in canonical row encodings.
Row fetches resolve the handle to a digest, then look up the actual tuple in the
canonical content tree. Both trees physically live in the relation's chunk container.

Index keys encode indexed values in SQL order for the supported types, followed
by the canonical row digest as a tie-breaker. Values contain occurrence count and row
bytes. Null position and descending direction are part of the key encoding;
variable byte strings use escaped terminators. No TID enters the canonical index
root. This initially overstores whole rows, including columns not in the index.
Consequently updates to any column can change every index root; projection to
minimal row references is future work.

Index scans traverse the persisted ordered tree and resolve canonical row keys
to snapshot-visible table handles. Index-only scans can supply projected values,
but still consult the runtime handle map. They do not use the heap visibility map.
The initial scan implementation materializes all matching TIDs and reconciles
index rows with handles using nested loops; it is not a logarithmic point lookup.
Indexes are rebuilt from current canonical table content on mutation, with chunk
deduplication. Incremental index edits and bounded caching are future work.

### PostgreSQL pages, WAL and publication

The standalone library's `DGCHNK01` file backend remains available and tested.
The SQL adapter instead stores the same `DGPTR001` nodes in PostgreSQL-managed
relation pages. It does not open a second chunk file or publish an external
manifest. Generic WAL full-page records recover chunk fragments and root records;
PostgreSQL owns buffer writeback, WAL-before-data ordering, relation creation,
transactional truncate/drop, backup-visible files and commit durability.

Each page contains one item: a 56-byte adapter header followed by a chunk fragment
or an 80-byte root version. The header identifies record kind, chunk length,
fragment offset and SHA-256 address. A root version contains XID, command ID,
canonical content hash, runtime handle-tree hash and next handle. All encoded
integers are big-endian. This experimental container format is not frozen.

All new chunks are logged before their root version. Root records are provisional
until their transaction commits. Reads select the latest root visible to their
PostgreSQL snapshot; own writes obey command IDs. Aborted/subaborted and prepared
uncommitted roots remain invisible. Historical handle lookup supports executor
SnapshotAny requests such as DELETE RETURNING without reintroducing deleted
rows into ordinary scans.

Table and index root records, along with ordinary catalog/heap changes, share the
SQL transaction outcome. There is no independently mutable latest-root file and
no postcommit rename gap. This is the initial relation-local working-set
publication mechanism, not a database-wide branch manifest. Named refs,
versioned schema roots and historical commit lookup remain phase 4 work.

### Concurrency and retention

Writes take a transaction-duration ExclusiveLock on the table. Readers may
continue with immutable snapshot roots. This deliberately serializes writers;
concurrent lock upgrades can deadlock and PostgreSQL detects those deadlocks.
A writer encountering a committed root newer than its active snapshot raises a
serialization error instead of merging whole roots or exposing snapshot phantoms.
This is conservative rejection, not full PostgreSQL row-level conflict behavior.

All chunks and root versions are retained. There is no VACUUM/freezing, GC,
compaction or snapshot-aware reclamation. The relation retains its initial
relfrozenxid; this implementation must not be operated indefinitely across XID
wraparound. VACUUM fails explicitly, rather than pretending to advance a safe
freeze horizon. A production retention/freezing protocol is required before
long-lived deployments. Prepared-transaction recovery has a basic integration
test, not the complete phase 3 concurrency certification.

### Core interfaces

Two optional callbacks are added to TableAmRoutine:

* `relation_index_am`: select/validate an index AM before opclass selection.
* `relation_validate_alter`: validate ALTER TABLE before catalog changes.

The extension uses the second callback to reject tuple-schema changes whose
historical decoding has not been implemented. Heap leaves both callbacks NULL.
Existing planner search, executor operators, lock tags, caches and transaction
commit logic are unchanged. Index planner integration supplies capabilities,
operator classes and a generic cost estimate. Estimates are deliberately crude.

## Supported and deferred surface

Tested: permanent tables containing boolean, int2/int4/int8, text COLLATE "C",
and bytea; NULL and empty values; duplicate rows; large inline canonical values;
sequential scans; ordered and index-only scans; simple multicolumn indexes with
ASC/DESC, NULL ordering and INCLUDE; immediate unique/primary-key constraints;
insert/update/delete and RETURNING; joins; transaction/savepoint rollback;
transactional TRUNCATE; retained reader snapshots and crash recovery.

Explicitly unsupported: other SQL types/collations, expression/partial indexes,
custom comparators, other index families, deferred uniqueness, ON CONFLICT,
row locking, concurrent index builds, SERIALIZABLE isolation, parallel scans,
TABLESAMPLE, ANALYZE, VACUUM/FULL, CLUSTER and most ALTER TABLE. CTID/tableoid are
available; heap transaction system columns are not supplied by virtual slots.
Temporary/unlogged tables are rejected. Runtime tuple-version chain following
is not implemented. General concurrent update/recheck behavior, foreign keys,
logical decoding/replication and complete DDL coverage are not claimed.

Roots contain table/index contents, not the external canonical schema identity;
hash comparisons assume the same schema and encoding contract. Standalone Dolt
branch/diff/merge APIs are not yet exposed through SQL by this extension.

## Tests

```
PGHOST=... PGPORT=... make -C contrib/dolt_prolly USE_PGXS=1 \
  PG_CONFIG=/path/to/installed/pg_config installcheck
```

The SQL regression covers supported operations and explicit rejections. TAP tests
start separate real servers, compare mixed edit histories with heap results,
check table/index canonical roots, retain a reader snapshot across another
backend's commit, reject a stale writer, exercise prepared mixed heap/prolly
transactions, and restart after immediate shutdown with committed, aborted and
fragmented data. Run the full core regression suite after changes to the policy
callbacks. The standalone C tree/file tests remain in `src/doltgres/prolly`.
