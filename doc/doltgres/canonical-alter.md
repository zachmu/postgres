# Branch-local ALTER and parallel schema publication

Checkpoint: 2026-10-22, branch `zachmu/canonical-alter`.

This extends the canonical SQL write gate with COMMENT ON TABLE/COLUMN, plain
nullable ADD COLUMN, and DROP COLUMN of unindexed columns. DROP removes the
column's NOT NULL constraint when present. Indexed-column DROP, CASCADE, defaults,
explicit collation clauses, generated/identity columns and multiple ALTER actions
remain rejected. ALTER TYPE, renames, general dependency cascades and schema merges
remain future work. Empty COMMENT text removes the comment, matching PostgreSQL.

Supported ALTER on an existing prolly table automatically transitions legacy
native capture to canonical publication if necessary. It captures current working
schema/data, initializes the ordinary empty repository only if absent, and sets
the canonical-catalog marker. It creates no user commit and needs no checkout or
materializer call. This one-time bootstrap uses the existing coarse administrative
locks and capture restrictions; mixed unsupported database objects can prevent
capture. Native heap DDL and the isolated old projection test fixture keep their
existing paths. Comments before activation retain native capture behavior.

## Schema and row publication

The caller must own the table. A private descriptor pins its old definition,
working ref and resource binding under the SQL snapshot. We load the latest
admissible branch root, construct a canonical schema revision, update its binding,
and append rewritten table/index roots in the same PostgreSQL transaction. An
imported file's physical pg_class row now validates only its allocation and AM,
not the versioned tuple layout. Canonical schemas supply the layout.

Both ADD and DROP eagerly rewrite packed rows. ADD appends a NULL field; DROP
removes a field and compacts the remaining ordinals. This deliberately avoids a
second, lazy tuple-layout translation contract during the experiment. Index
column mappings are reconstructed from symbolic definitions, and surviving indexes
are rebuilt, including indexes whose ordinals shifted. DROP of an indexed column
is rejected before publication pending explicit index/dependency removal support.

Repacking changes each row's content hash. Rows that become byte-identical are
coalesced by summing their occurrence counts, with overflow and hash-collision
checks. Retained roots and canonical schemas preserve old layouts and dropped
values. ADD followed by DROP of the new column restores the same canonical schema
and content hash. Metadata-only COMMENT updates schema/membership without rewriting
table rows. No storage resource ID or file is allocated for these schema revisions.

Old SQL snapshots retain their old schema binding and root. Prepared statements
are reanalyzed under the existing context dependency rules; a SELECT * whose
result type changes raises PostgreSQL's cached-plan result-type error. Unsupported
FOR UPDATE on a versioned read now rejects explicitly for both qualified and
unqualified locking clauses; the unqualified path previously ignored the custom RTE.

## Required concurrency contract

Two sessions must be able to change different branches while both transactions
remain open, including when the branches share table/index files and produce
identical new chunks. Three formerly coarse synchronization points were changed:

1. Canonical writers hold a branch/resource object lock for the transaction,
   plus a compatible physical RowExclusiveLock. Same-branch writers remain
   serialized. The branch component uses a 32-bit digest; collisions conservatively
   cause false contention, never incorrect sharing of roots.
2. File append holds ShareUpdateExclusiveLock only around one state append, then
   releases it. This keeps fragmented chunk records contiguous without retaining
   a file-wide writer lock until COMMIT. Buffer locking, WAL-before-data ordering
   and PostgreSQL transaction visibility still govern pages and roots.
3. Binding/resource catalog mutations use compatible RowExclusiveLock relation
   locks; tuple and unique-index locks coordinate updates to the same metadata.
   Different branches no longer acquire a conflicting catalog-wide mutation lock.

The immutable-object catalog also needed a change. A unique index on content hash
made two independent transactions publishing the same new bytes wait for each
other's outcome. pg_dolt_object now permits byte-identical physical copies, with a
random UUID primary key identifying each heap row and a nonunique hash index.
The UUID is local storage metadata: it is not included in the object encoding,
canonical hash, schema, table key or root. pg_dolt_object_visible presents DISTINCT
hash/kind/payload/children under the caller's snapshot. All product object reads
use that logical interface; refs remain uniquely keyed and use the same CAS rules.
No transaction is independently committed behind the user's back.

This trades redundant heap/TOAST storage and DISTINCT-read overhead for independent
publication. Sequential insertion still reuses visible content. Concurrent copies
need future reclamation; changing the physical uniqueness rule does not change
logical object identity. Catalog privileges still prevent this from becoming a
public data oracle. Retained object GC and relation-root freezing remain unresolved.

Version administration still uses a coarse database object lock. Ordinary
activated canonical ALTER does not take it, so another branch can also dolt_commit
while the first branch's DDL transaction remains open. The one-time legacy
activation is an explicit exception to fully parallel steady-state operation.
This milestone does not implement same-branch row-level concurrent writes.

## Verification

All 29 selected native Meson groups passed: test_prolly, test_doltgres, the 231-test
core regression suite and the 122-test isolation suite. The final targeted ALTER
suite passes 42 checks, including:

- A keeps COMMENT/ADD and DML uncommitted while B applies the same nullable layout,
  writes, SQL-commits and dolt_commits; both branches share existing storage.
- Independent DROP rewrites, another branch committing while its peer rolls back,
  and overlapping large writes with fragmented chunks.
- Old snapshots, prepared-result-type checks, cross-branch incompatible row types,
  shifted primary/unique indexes, duplicate multiplicity and canonical round trips.
- Owner checks, metadata-only rollback, savepoints, restart, standby reads, and
  crash rollback of uncommitted schema plus data.

The manifest test now verifies nonblocking identical-object publication and
snapshot-correct logical deduplication, replacing its previous expectation that
such inserts wait. Obsolete project guard tests now exercise unsupported ALTER TYPE.
Core test SQL was not changed; rules.out's fork-added catalog listings were updated
for the logical object view and the two Dolt views that consume it.

Doltgres external-server tests passed 12 selected suites, 27 scenarios and 207 leaf
assertions. New TestBranchCatalogAlter (three scenarios, 25 assertions) also passes
against Doltgres itself. Its cases cover unprepared plain ALTER, branch/history
isolation, and duplicate-row preservation. The existing TestAlterTable/Drop_Column
scenario additionally passes all five enabled assertions unchanged; its existing
IF EXISTS assertion remains upstream-skipped. This is selected compatibility
coverage, not the full Doltgres suite. Harness behavior was not changed.

Next: dependent index removal and fuller DDL/dependency closure; improve stored
catalog-view/utility coverage; optimize eager rewrites and index rebuilding only
after correctness gates; design physical duplicate reclamation and same-branch
row-level concurrency. Root planning documents remain unchanged per AGENTS.md.
