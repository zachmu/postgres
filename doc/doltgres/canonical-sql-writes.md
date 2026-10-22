# Canonical SQL writes and same-name CREATE gate

Checkpoint: 2026-10-22, branch `zachmu/canonical-resource-writes`.

After branch catalogs have been activated by checkout, plain permanent table
CREATE publishes a canonical schema and branch membership, allocates table/index
storage resources, and records their bindings in one PostgreSQL transaction.
It creates no physical pg_class, pg_attribute or pg_type schema rows for those
objects. Reads do not allocate files or require a materialization command.
Bootstrap/pre-activation tables still use native catalog capture and imported
resources; eliminating that bridge is not part of this checkpoint.

## Definitions, resources and executor authority

The canonical database object identifies table and index definitions. A CatalogView
vends native catalog tuples and backend-local relation/type handles for a specific
definition context. Such handles are not persistent resource IDs or shared lock
identities. A write opens a private Relation descriptor carrying the selected
branch, canonical schema, retained base root, registered SQL snapshot and stable
resource locator/lock identity. Its indexes are bound under the same context and
owned by that descriptor. Cached canonical definitions are never modified to
point at a mutable session-selected file.

Resource IDs describe actual storage allocations, not every commit or schema
revision. A newly created table generation gets storage; branches can share that
storage, and old definitions can import retained content into branch-tagged roots.
Tests also bind incompatible schemas to a single independently allocated resource.
The pg_dolt_binding/pg_dolt_storage records remain local operational metadata,
separate from portable canonical schemas and content.

Ordinary INSERT/UPDATE/DELETE use native executor target slots and table/index AM
callbacks. Parsing carries the selected branch into the target RTE; execution
checks that its bound definition still matches. COPY uses a similarly bound
resource. regclass lookup and supported ordinary catalog scans use the selected
branch. LOCK TABLE resolves its resource separately after native ACL checks.
Prepared-plan locking skips backend-local definition IDs.

## Publication and concurrency

Writers pin the snapshot-visible working-ref and binding tuples with Share tuple
locks and lock the stable storage resource. This prevents publication/rebinding
from overtaking an admitted writer. An obsolete snapshot fails with a serialization
error. Relation roots retain the existing PostgreSQL XID/command-ID publication
rules, WAL and recovery integration. Transaction abort and savepoint rollback
undo schema membership, resource metadata and roots together.

Separate branch CREATE operations allocate separate resources and do not block
ordinary writes to the other branch's table. This is not row-level concurrency:
writes to a shared resource remain serialized, including across branches sharing
that resource. Administrative version commands still have a coarse database
object lock. These are explicit remaining concurrency limitations.

Canonical capture enumerates canonical database membership, then reads each
member's snapshot-visible storage root. Merge and conflict resolution also
use canonical membership and bound resources, so they cannot omit tables that
have no physical schema rows. Snapshot command IDs must advance after table and
index publication, once bound resource pins have been released; otherwise a
fast-forward can capture an old index root and leave a falsely dirty working set.

## Supported surface and limits

This gate covers plain permanent tables, supported built-in stored types, NOT NULL,
immediate PRIMARY KEY and UNIQUE constraints, DML, COPY, DROP/recreate, data merges,
conflict resolution and revision reads. Schema merge remains rejected. Defaults,
identity/generated columns, user-defined column types, foreign keys, checks,
partitioning/inheritance, event triggers and general ALTER/CREATE INDEX remain
outside the activated canonical CREATE path. Index key types/collations retain
existing prolly restrictions. Automatically chosen index names have a restricted
collision policy; explicit names can disambiguate distinct constraints.

Canonical write targets currently use sequential plans, and index maintenance
rebuilds trees. This is correctness machinery, not a final performance design.
Serializable and parallel writes remain unsupported. The complete catalog/type/
dependency/utility surface is not certified by this checkpoint. Supported catalog
families are supplied by the provider; stored views such as pg_tables/pg_indexes
that would expose physical definitions are rejected pending contextual rewrite
support. Bootstrap/global catalogs remain native. More DDL, stored-query binding
and introspection coverage must extend the same explicit context contract.

## Verification

An assertion-enabled build passed all 28 selected Meson groups: test_prolly,
test_doltgres, core regress and core isolation (1,867 reported subtests, including
231 core regression and 122 isolation tests). No upstream PostgreSQL test was
changed. Project test expectations were updated where they described obsolete
physical regclass leakage, same-name CREATE rejection or old diagnostic wording.

New TAP suites:

- 004_storage_writes: 36 checks of native AM writes, unique indexes, permissions,
  retained-root import, concurrent metadata pins, rollback, stale snapshots,
  resource identity, crash recovery and standby reads.
- 005_canonical_sql: 27 checks of actual SQL CREATE/DML/COPY, no physical schema
  rows, simultaneous incompatible branch schemas, prepared statements and cursor
  stability, ordinary catalog/regclass consistency, rollback/savepoints, restart,
  standby and crash cleanup; unsupported catalog views fail closed.
- 006_canonical_merge: five checks of clean and conflicting merges, conflict
  resolution and recovery for a canonical-created table without physical schema.

Doltgres external-server validation passed 11 selected suites, 24 scenarios and
182 leaf assertions: TestAsOf, TestDoltMergeData, TestPostgresMergeConflictStatus,
TestBranchCatalogCreate, TestBranchCatalogDrop, TestBranchCatalogSameName,
TestCatalogHistory, TestProllyVersionCompatibility, TestNativeVersionedQueries,
TestNativeHistoricalCursors and TestRevisionWorkingSets. This is not the full
Doltgres suite. Existing external-server harness and tests were used unchanged.

Next gates: broaden canonical DDL and namespace/schema changes; complete contextual
stored catalog views and remaining utility lookups; then remove the bootstrap/import
bridge. Row-level concurrency and index execution/maintenance performance remain
separate substantial work. Root planning documents were left unchanged per AGENTS.md.

## Parallel canonical ALTER checkpoint (2026-10-22)

[Branch-local ALTER](canonical-alter.md) records the next gate: COMMENT, nullable
ADD COLUMN and unindexed DROP COLUMN, automatic legacy activation, and parallel
DDL on branches sharing storage. It supersedes the earlier resource-wide writer
serialization and physical hash-uniqueness descriptions. Logical hashes and
PostgreSQL transaction publication remain authoritative.
