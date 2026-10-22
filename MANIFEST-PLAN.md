# Database manifest and version-control publication plan

This sub-plan accompanies [PLAN.md](PLAN.md) and the
[row-level tracking plan](ROW-LEVEL-TRACKING-PLAN.md). It records the next
implementation direction; the foundation status is recorded below.

## Direction

The branch-view design must ultimately support references to multiple branches
of the same table in a single query. A session's selected branch is only the
default: explicit branch or commit bindings belong to individual table
references and must survive planning, index lookup, tuple fetch and rescans.
Neither a shared relation cache entry nor a session-global branch variable is
sufficient for that future capability. Independent per-session views are in
the current branch-and-merge milestone; cross-branch queries and historical
schema projection remain subsequent work.

Implement the per-database manifest using PostgreSQL heap-backed internal
metadata and existing MVCC, locking and WAL infrastructure. Branch HEAD and
working-set updates should obey PostgreSQL transaction visibility and conflict
handling, with one SQL transaction able to update multiple refs atomically.
There must be no separately committed manifest file or second commit authority.

Distinguish immutable database states and commit objects from mutable refs:

- A database state identifies versioned schema, table contents and index roots.
- A version-control commit references a database state, parent commits and
  history metadata.
- The manifest maps branch heads, tags and working sets to their targets.
  Working sets represent durable changes not yet recorded in a version-control
  commit. The logical manifest need not be one physical row or pointer.

SQL commit publishes transactional changes; `dolt_commit()` creates a history
node and changes the applicable ref within a defined SQL transaction boundary.
For this first slice, commands whose semantics require committing (including
commit and future merge) must run as standalone statements outside explicit SQL
transaction blocks; do not implicitly commit an existing PostgreSQL transaction. PostgreSQL heap metadata supplies useful primitives, but does not
by itself establish the complete publication protocol.

## Working-set publication decision

Use Dolt-style three-way merging for publishing changes to working-set refs:

```text
base   = working state identified by transaction-start metadata
ours   = that base with the transaction's computed changes
theirs = latest working state selected for publication
result = merge(base, ours, theirs)
```

PostgreSQL locking, conflict handling, visibility and commit semantics govern
this reconciliation. They are a semantic layer over the merge mechanism, with
integration at read, mutation and publication time, not only a check after merge.
The access methods must supply row/version metadata and executor callbacks;
heap-backed refs do not automatically provide those guarantees for prolly rows.

- Preserve unrelated committed changes while applying the transaction's own
  validated changes. Do not rerun arbitrary SQL expressions during merge.
- Apply PostgreSQL same-row waiting/rechecks, uniqueness, DDL coordination and
  isolation rules. A structurally successful merge can still be invalid SQL.
- Keep read views separate from the publication result. Read Committed can use
  several statement snapshots; transaction-start metadata alone does not describe
  every read. Later-visible rows and predecessor versions need explicit tracking.
- Validate that the publication target has not advanced during merge; retry or
  coordinate as required. Multiple ref changes share the SQL commit outcome.
- Retain enough metadata/state to construct snapshot-correct views of roots
  containing several transactions. A publisher XID alone is insufficient.
- Coordinate only the necessary publication work; do not hold a branch-wide
  writer lock throughout ordinary transactions.

This settles the architectural direction, not the exact row-delta/root-selection
algorithm or its proof. The existing foundation does not yet perform this merge
or connect ordinary DML to working-set ref moves.

## Database contents and discussion status

The full database root must cover [PLAN.md §3](PLAN.md#3-data-indexes-and-schema-are-versioned-together),
including empty namespaces, types, functions/procedures, triggers, constraints,
dependencies and sequence definitions as well as tables and indexes. The first command slice adds actual namespace descriptors, including empty
namespaces, and limited table/index schema serialization. Routines, custom types,
sequences and the rest of the full schema remain future work; the typed model
and canonical serializers must expand together.

The architectural discussion is sufficient to proceed: use heap/WAL metadata,
retain immutable history independently of relation lifetime, and reconcile
working sets through three-way merge governed by PostgreSQL semantics. Remaining
items are engineering specifications and acceptance gates, not a requirement to
reopen that direction: canonical payload formats and capture APIs, historical
schema/DDL consistency, snapshot eligibility and row-version lifetime, precise
publication synchronization, and public command/authorization behavior. Logical
history backup/import also remains deferred; physical backups preserve history.

## Implementation order

1. **Define the shared publication contract.** Coordinate this with milestone 1
   of the row-level plan: ref identity, snapshot resolution, own-write visibility,
   expected-target validation, lock ordering, multi-ref atomicity, savepoints,
   abort and recovery. Specify how refs and table/index/schema roots share one
   PostgreSQL transaction outcome.
2. **Build durable metadata and immutable objects.** Implement heap-backed refs,
   commit objects and database-state descriptors. Keep local OIDs, physical
   locators and transaction bookkeeping outside canonical content hashes.
   Decide durable chunk ownership and retention before allowing history to
   outlive a table's current relation.
3. **Exercise a narrow version-control slice.** Start with commit creation,
   branch/tag creation, log and diff through `dolt_` functions. Historical SQL
   reads and checkout require branch-aware schema resolution, not just saved
   table roots; gate them on that integration. Preserve explicit prototype
   concurrency restrictions while validating the slice.
4. **Complete row-level concurrency against this model.** Add private changes,
   row/version tracking and validated, snapshot-correct publication. Remove
   transaction-duration writer serialization only after its replacement passes
   the row-level plan's gates.

This brings foundational manifest and version-control work forward from phases
4–5, before the full Phase 3 concurrency implementation. It does not waive those
phases' remaining schema, isolation or recovery requirements. WAL correctness is
required at every step; custom WAL record formats and WAL optimization can wait.

## Concerns and acceptance gates

- **Snapshot correctness:** resolve mutable refs under the applicable SQL
  snapshot. Visibility of a root's publishing transaction does not prove that
  every transaction incorporated into that root is visible. Test old snapshots,
  out-of-order commits and own writes together with the row-level protocol.
- **Contention and lost updates:** a working-set row updated by every writer can
  become a branch-wide transaction-duration lock. Define validation/rebase and
  publication coordination before adopting that as the permanent write path.
  A single immutable database-state root does not require a single long-held
  writer lock. Concurrent ref moves must have explicit wait/recheck or conflict
  outcomes, rather than blindly replacing a previously read target.
- **History lifetime:** existing chunks reside in table/index relations. A
  historical ref must remain readable after DROP, TRUNCATE or relation rewrite.
  Establish independent durable ownership or retention; a hash alone does not
  preserve its target. Retain schema needed to decode historical values too.
- **Atomic recovery:** test failures between object persistence, ref changes
  and SQL commit. After recovery, committed refs must resolve to complete
  objects; aborted changes must remain invisible. Test multi-ref updates mixed
  with ordinary heap/catalog changes, savepoint rollback, and prepared
  transaction resolution as the corresponding capabilities become supported.
- **Identity and reclamation:** separate portable content identity from local
  runtime mappings. Define reachability from refs, history, active snapshots and
  in-flight transactions before reclaiming objects; the initial implementation
  may retain everything explicitly.

The first milestone should deliver a reviewable publication state machine and
real-server interleaving/crash tests alongside the metadata implementation.
Full checkout, merge and historical-schema support remain separately gated.

## Foundation milestone implemented

Steps 1–2 now have an internal implementation: per-database heap catalogs for
refs and immutable typed objects, snapshot-based ref reads, revision-token
compare-and-swap, and export of reachable canonical table/index chunks into
relation-independent history storage. PostgreSQL owns WAL, commit, rollback,
locks and prepared transaction recovery. The detailed implementation contract
and API are in [doc/doltgres/manifest.md](postgres/postgres/doc/doltgres/manifest.md).

This is the metadata/publication foundation, not completion of the separate
row-level identity/publication milestone. The generic object API accepts trusted caller-supplied payloads; the command
layer described below now supplies limited canonical schema capture. Branch-aware
SQL resolution and live working-set publication remain to be built.
In particular, ordinary DML does not update a shared ref row. Its future
snapshot-visible delta/base-root protocol must be settled before that connection
is implemented. Concurrent creation of identical new objects may wait or require
retry; this metadata ingestion path is not yet the final row-write chunk store.

History survives source relation removal and is covered by physical backups.
Logical pg_dump omits manifest history; GC and history import/export are later
work. Fresh initdb is required. The progress report is
[Manifest foundation](reports/manifest-foundation.html).


## First command slice implemented

Step 3 now provides native `dolt_add`, `dolt_commit`, `dolt_branch`, `dolt_tag`,
`dolt_log` and `dolt_diff_summary`, plus log/tag views. Shapes and supported flags
follow the checked-out Doltgres/Dolt implementations. Unsupported operations
error explicitly. See the [command contract](postgres/postgres/doc/doltgres/version-commands.md)
and [progress report](reports/manifest-commands.html).

- The live SQL branch remains main. Creating branch/tag refs does not switch
  tables. Checkout, historical SQL and branch projections remain gated.
- Commit, branch and tag require standalone SELECT outside explicit transactions.
  Add is a standalone SELECT but can participate in BEGIN/ROLLBACK. Commit without
  -A uses staged state; -A captures all live supported objects. Ordinary DML does
  not advance working refs; WORKING diff captures current live state.
- Capture uses one refreshed Read Committed snapshot for metadata and table/index
  roots, with temporary catalog/relation DDL locks. Source chunks are copied into
  history before releasing capture locks. Live capture under Repeatable Read or
  Serializable is rejected until historical schema binding is implemented.
- Namespace, table/index definition and content identities are canonical and
  symbolic; owners/ACLs use role names, never OIDs. Empty namespaces participate.
  The supported subset excludes routines, custom types, sequences, defaults,
  generated columns, triggers, views and other unsupported schema properties.
- Ref changes and objects share PostgreSQL commit/WAL. Stale publication fails;
  it does not yet merge concurrent working sets. The three-way publication
  decision above remains the direction for step 4 and the row-level plan.
- APIs remain superuser-only; hashes use this engine's SHA-256 format. Fresh
  initdb is required. Physical backups include history; logical dumps omit it.

Next gates: row-level delta/base-root publication and concurrency; broaden schema
serialization and branch-aware binding before checkout/historical SQL; add row
read/diff APIs over retained content and then merge with PostgreSQL conflict and
constraint checks. No change to the agreed three-way reconciliation direction.

Current-slice hardening is complete: 28 additional real-server assertions cover
prepared staging, savepoints, cancellation/crash between ref moves, and capture
error cleanup. See [publication hardening](reports/manifest-hardening.html).
External Doltgres compatibility verification follows; row-level concurrency
remains the next separate implementation milestone.

The first external Doltgres compatibility pass is complete; see the
[verification report](reports/doltgres-compatibility.html). The supported profile
passes 549 query assertions, with 20 focused version-control assertions also
validated against in-process Doltgres. The subsequent bootstrap correction
removes the extra CREATE DATABASE commit: it is an undesirable Dolt storage
library artifact, not a semantic requirement. Shared tests accept either
bootstrap count and verify user-created commits independently. Existing history
is not rewritten. See [the correction report](reports/bootstrap-history.html).
Explicit exclusions and heap controls distinguish unsupported paths
from PostgreSQL behavior differences. These checks do not complete row-level
publication or branch-aware schema resolution.


## Branch and merge data milestone — October 21, 2026

The native branch-and-merge slice now implements independent session data views,
checkout, merge-base discovery, fast-forward/no-op and unchanged-schema three-way
merges. Conflicts return status flags and preserve heap/WAL-backed evidence and
pending parents for resolution or abort. Schema merges remain excluded.
See the [implementation contract](postgres/postgres/doc/doltgres/branch-merge.md)
and [milestone report](reports/branch-merge.html) for decisions, tests and limits.

This supersedes earlier notes that SQL could only access main. Each relation has
branch-tagged runtime roots; a branch without local roots reads its retained
HEAD state. Session selection supplies a default at the storage binding boundary.
A future query referencing multiple branches of the same table still requires
per-table-reference bindings in planning, scans and tuple fetch, plus historical
schema projection. Neither capability is claimed by this milestone.

Schema catalogs remain shared. Checkout verifies identical definitions and
activates a broad database DDL/TRUNCATE freeze. This is temporary, not a substitute
for branch-aware catalogs. Merge/resolution use ordered relation locks; ordinary
writers still serialize per table across branches. Row-level delta/base-root
publication, SSI, row locking and dirty-working-set reconciliation remain separate
work. A merge currently requires clean working/staged contents.

Primary-key rows merge by key and then by column; JSON objects merge recursively.
The Doltgres oracle classifies changes by both branches to the same keyless row's
cardinality as a conflict, even when final counts match. We match that rule, rather
than summing indistinguishable occurrences. The intentional difference is returning
a non-aborting conflict flag. Unique constraint collisions conservatively preserve
the destination table and record a whole-table conflict; choosing a side replaces
that entire table. Finer constraint artifacts are deferred. Pending merge objects
and CAS refs use existing PostgreSQL heap, MVCC and WAL, without a second manifest.

Catalog binding now uses native in-memory versioned descriptors and lazy retained
scans; see [CATALOG-BINDING-PLAN.md](CATALOG-BINDING-PLAN.md). The persistent
materializer and projection mapping were removed. Branch-qualified reads select
snapshot-visible working roots; AS OF resolves committed revisions. No query-time
catalog publication is required. Writable branch-local schema publication and
its dependencies remain open work, coordinated with manifest CAS and SQL MVCC.
