# Row-level tracking and concurrent publication plan

This sub-plan expands [PLAN.md §13–15](PLAN.md#13-mvcc-locking-and-isolation-integration).
It records the agreed first-pass direction for Phase 3. It is a design and set of
implementation gates, not a claim that these mechanisms already exist. Detailed
publication and snapshot algorithms must be resolved in milestone 1 before
replacing the current locking scheme.

## 1. Objective and current baseline

Retain root-based views of immutable canonical contents while adding row-level
concurrency control outside canonical trees. Remove the transaction-duration
table writer lock: independent row edits must proceed concurrently without lost
updates or whole-table serialization failures.

The adapter now lives in core as additional built-in access methods, with heap
remaining the default; extension packaging has been removed. It stores relation-local canonical/runtime root records with
XIDs and command IDs, selects visible roots, serializes writers with an
ExclusiveLock, and rejects stale writers. This is temporary scaffolding. Normal
PostgreSQL relation locks for DDL coordination remain appropriate; removing the
writer lock does not mean eliminating relation locks altogether.

Continue using PostgreSQL's transaction coordinator, snapshots, lock/deadlock
framework, executor and WAL-controlled transaction outcome. The final database-wide
working-set/ref foundation is now being brought forward under the
[manifest plan](MANIFEST-PLAN.md), using heap-backed metadata and existing MVCC
and WAL. Define its publication contract together with milestone 1 here, then
validate a narrow version-control slice before completing row-level concurrency.
Full branch-aware schema integration remains a later gate. The protocol here
must support atomic multi-relation and multi-ref transactions without replacing
the table writer lock with a transaction-duration branch-wide writer lock.

The manifest foundation now supplies snapshot-based ref reads, expected-token
moves and durable immutable objects; see its
[publication contract](postgres/postgres/doc/doltgres/manifest.md). Explicit ref
moves share the SQL transaction with heap and prolly changes. Ordinary DML is
not connected to ref moves. Milestone 1 here still must settle live root/delta
eligibility and version lifetime before implementing concurrent row publication;
metadata CAS alone does not solve those requirements.

## 2. State and identity model

| Component | Responsibility |
|---|---|
| Canonical table/index roots | Represent stored values and index contents without allocation-dependent row IDs, XIDs, locks or command IDs. Share unchanged chunks. |
| Stable runtime row identity | Identify each logical row occurrence across updates. Distinguish identical duplicate occurrences in keyless tables. |
| Runtime version tracking | Identify individual versions, creating/updating/deleting transactions, command boundaries and successor versions. Support visibility checks needed for conflict resolution and executor rechecks. |
| Row-lock state | Track lock strength, ownership and compatible lockers; wait through PostgreSQL's lock/deadlock machinery. |
| Transaction-local changes | Record inserts, updates and deletes, their expected predecessor versions and associated index changes. Provide own-write visibility and savepoint rollback. |
| Publication state | Validate changes against the latest publishable committed state and associate resulting roots and runtime metadata with one PostgreSQL transaction outcome. |

Keep stable row identity distinct from version identity and the executor-visible
TID representation. Decide explicitly whether a TID denotes a version or a stable
occurrence, and how callbacks follow successors. An update must preserve enough
identity to find and lock the same logical row without making canonical hashes
depend on its runtime handle. Key changes and delete/reinsert require explicit
identity rules. Identical duplicate rows remain individually lockable/deletable.

Row metadata is outside canonical hashes, not outside durability requirements.
Persistent identity/version records need WAL and recovery. Ordinary live locks
may be reconstructed or discarded according to transaction outcome; prepared
transactions require recovery of their lock/conflict ownership.

The current policy rejecting non-portable identifiers in stored SQL values
remains in force. Runtime identities used internally do not grant permission to
serialize those identifiers into canonical row or index contents.

## 3. Read and mutation model

A statement reads its snapshot's committed contents plus its transaction's
eligible private changes. Command IDs distinguish earlier statements from changes
made by the current command. Subtransaction ownership permits rollback to discard
only the appropriate changes and release/retain locks according to PostgreSQL
semantics.

A writer identifies a row occurrence and expected version, acquires the
appropriate row lock, and validates or follows that version before recording a
private mutation. Row tracking handles conflicts; it does not require adding
heap xmin/xmax fields to every canonical tuple.

- **Read Committed:** use statement snapshots. After waiting for a conflicting
  updater, follow its committed successor and supply the table-AM results needed
  for PostgreSQL's executor to re-evaluate conditions through EvalPlanQual.
  A concurrent delete may instead mean there is no surviving row to modify.
- **Repeatable Read:** retain the transaction snapshot. Conflicting modifications
  since that snapshot cause serialization failure where PostgreSQL requires it;
  unrelated row modifications must not automatically cause failure.
- **Serializable:** additionally integrate PostgreSQL SSI/predicate-lock behavior,
  including missing-key and empty-range reads. Row locks alone do not prevent
  write skew or phantoms. Keep this isolation level explicitly unsupported until
  the corresponding gate passes.

Match the table AM's TM_Result, failure information, tuple-lock and version-following
contracts. Cover FOR UPDATE, FOR NO KEY UPDATE, FOR SHARE, FOR KEY SHARE, NOWAIT,
SKIP LOCKED, cancellation, deadlocks and lock upgrades. Do not substitute
version-control merge/conflict resolution for SQL concurrency semantics.

## 4. Publication and snapshot correctness

Transactions publish validated changes, not an unconditional replacement root
built from their original snapshot. For disjoint edits:

```text
Starting state: R0
A privately updates row 10; B privately updates row 20.
A publishes its change: R0 -> R1.
B applies its validated change to the publication base R1: R1 -> R2.
R2 contains both edits. B's earlier reads still obey B's snapshot.
```

Use Dolt-style three-way merge for this reconciliation, with transaction-start
metadata identifying the base, the transaction's computed changes forming ours,
and the latest working set forming theirs. PostgreSQL's row-conflict, constraint,
visibility and commit rules govern the merge. This is separate from the policy
for user-requested version-control merges. See the
[manifest publication decision](MANIFEST-PLAN.md#working-set-publication-decision).
Transaction-start metadata must be supplemented by statement snapshots and
predecessor/version tracking when Read Committed observes later changes. This
direction does not remove the snapshot proof or publication validation below.

Do not re-execute arbitrary SQL or expressions during publication: preserve the
computed private changes, applying PostgreSQL's required row recheck behavior at
the appropriate execution stage. A change must validate its predecessor/version,
constraints and relevant index conflicts before it can publish.

Root advancement will require brief synchronization, reservations or compare-and-
retry. Build expensive immutable chunks outside the critical section where
possible. Specify retry bounds, fairness, cancellation and ordering for transactions
touching multiple relations. Measure contention; moving the table-wide lock to
an equally long commit critical section is not an acceptable result.

### Required snapshot proof

A newer root may include several transactions' changes. Testing only the XID on
that root is not sufficient unless every incorporated change is also visible to
the reader. PostgreSQL snapshots can exclude transactions that were still active
when the snapshot was taken, and commit order is not XID allocation order.

Milestone 1 must specify how a reader obtains a complete snapshot-correct view:
for example, by proving eligibility of a root's incorporated transaction set and
applying visible deltas, or by constructing a snapshot-specific root from retained
state. Choose and test a concrete algorithm; do not assume that selecting the
latest individually visible root automatically solves this. Include snapshots
created between publication reservation, root construction and SQL commit.
Own-write overlays must preserve this same invariant.

### Atomicity and recovery

Table roots, index roots, durable row-version metadata and ordinary heap/catalog
changes share PostgreSQL's transaction decision. A reader must not observe a new
table root with an old corresponding index state within one snapshot.

Specify publication states, lock/reservation ownership, WAL ordering, abort
cleanup and idempotent recovery before implementing the commit path. Account for
out-of-order commits, asynchronous commit, failures after logging chunks but before
commit, and failures after durable commit but before in-memory publication finishes.
Do not introduce a separately committed manifest file. Generic WAL may remain
sufficient; custom WAL records are an optimization/integration decision, not a
prerequisite assumed by this plan.

Prepared transactions must retain enough changes, predecessor expectations,
locks and unique-key reservations to commit or abort correctly after restart,
without blocking all unrelated writes to a table.

## 5. Indexes, uniqueness and retention

Maintain table and index changes together. Incremental index mutation should
replace whole-index rebuilding on each row change. Concurrent inserts of an equal
unique key conflict even when the rows have different runtime identities.

Design unique-key reservations or equivalent tracking around the operator class's
SQL equality, collation and NULL semantics, not simply serialized byte equality.
Specify waiting and rechecking when a competitor commits or aborts, including
key-changing updates. Extend to speculative insertion/ON CONFLICT, deferred
constraints and foreign-key key-share interactions before claiming full Phase 3
compatibility. Existing restrictions on unsupported index types remain explicit.

Retain row versions, roots and successor mappings while required by snapshots,
active commands, row locks, prepared transactions and recovery. Specify handle
reuse and transaction-status/XID lifetime rules. Retention can initially be
conservative, but must not permit dangling TIDs, lost successor chains or unsafe
XID wraparound. Full GC remains later work; indefinite retention is not a
production lifecycle solution.

## 6. Milestones and acceptance gates

### Milestone 1 — Identity and publication contracts

Specify stable occurrence/version identities, TID adaptation, version-chain
lifetime, private changes, snapshot-root selection and the commit/publication
state machine. Resolve the snapshot proof above, multi-relation ordering and
commit reservation behavior. Identify necessary table-AM, transaction, lock,
WAL and recovery integration points and any required core changes.

**Gate:** reviewable state transitions and executable interleaving tests for
no lost updates, no invisible transaction leakage, no partial publication, and
correct own-write visibility. Record unresolved protocol choices explicitly;
do not remove the current lock before a safe replacement is specified.

### Milestone 2 — Private changes and disjoint writers

Implement transaction-local row/index changes and validated application to the
latest publication base. Remove transaction-duration table writer serialization
for the supported path. Preserve read snapshots and command visibility.

**Gate:** concurrent transactions modify disjoint rows in the same table without
waiting for one another's transaction end or losing changes. Their commits may
briefly coordinate publication. Independent row changes do not trigger the old
whole-root stale-snapshot error. Final canonical roots match equivalent serial
histories; snapshots see exactly their eligible changes.

### Milestone 3 — Same-row conflicts and executor integration

Implement row-lock strengths, waiting, successor following, TM_Result reporting,
EvalPlanQual rechecks, delete/update races, key changes, savepoints and deadlocks.

**Gate:** multi-session tests match heap behavior for Read Committed and
Repeatable Read, including individual duplicate rows, WHERE-clause rechecks,
RETURNING, NOWAIT/SKIP LOCKED, rollback after waiting, cancellation and lock
upgrades. Savepoint rollback restores the correct private changes and ownership.

### Milestone 4 — Concurrent uniqueness and atomic table/index publication

Implement equality-aware unique-key coordination, incremental index changes and
atomic multi-relation publication. Complete the associated constraint paths,
including speculative insertion, deferred checks and foreign-key interactions,
or keep them explicitly outside the supported intermediate milestone.

**Gate:** same-key insert/update races wait, succeed or fail as PostgreSQL requires;
abort releases reservations; no snapshot sees mismatched table/index states.
Transactions spanning multiple prolly relations and ordinary heap/catalog
changes preserve a single commit decision and do not deadlock through inconsistent
publication ordering. Full Phase 3 completion still requires its constraint scope.

### Milestone 5 — Recovery, prepared transactions and Serializable

Validate crashes at every publication transition, prepared transaction restart,
commit/abort resolution and runtime metadata recovery. Then complete SSI and
predicate-lock integration, including empty ranges and index/table scan paths.

**Gate:** committed changes survive and aborted changes remain invisible across
fault injection. Prepared transactions recover conflict ownership without
serializing unrelated table writes. Serializable tests reject dangerous
structures, including write skew and phantoms, consistently with PostgreSQL's
contract. State supported durability and retention limits explicitly.

## 7. Validation and implementation discipline

Use real PostgreSQL sessions and deterministic synchronization to compare prolly
and heap behavior. Cover disjoint updates, competing updates/deletes, duplicate
occurrences, duplicate-key races, isolation levels, deadlocks, savepoints,
prepared transactions and crashes during publication. Compare permitted outcomes
and visibility, not an arbitrary winner chosen by scheduling. Add performance
measurements for publication contention, retry rates and long transactions.

Implement milestones in scoped feature branches with incremental commits, merge
verified work locally into `zachmu/doltgres-rewrite`, and do not push without
instruction. Keep a concise progress report for each milestone. Update this
sub-plan as protocol choices settle; keep the main PLAN focused on direction,
phase gates and links to detailed sub-plans.


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

Coordinate per-reference root selection and writable catalog publication with
[CATALOG-BINDING-PLAN.md](CATALOG-BINDING-PLAN.md). Native versioned table bindings
now select independent working roots in one query and preserve snapshot visibility.
Historical scans use retained chunks without persistent projection. This does not
remove table-wide writer serialization or implement branch-local DDL. The former
persistent materializer has been removed entirely.

## Schema identity and concurrent DDL — 2026-10-22

Coordinate the [branch-local DDL gate](CATALOG-DDL-PLAN.md) with row publication.
Each bound reader and pending write must identify the schema against which its
rows, index keys and expressions are interpreted. Validate that identity before
publishing or rebasing changes; tuple-layout compatibility alone is not proof of
semantic compatibility. Conflicting DDL/DML must wait and recheck or fail explicitly,
not reinterpret old tuple bytes using a newer definition.

Readers on retained snapshots/cursors must keep their original metadata and data
valid while another branch evolves. Lock identity must distinguish branches while
still coordinating writers of the same logical branch object. Retention covers
schema/dependency objects as well as data chunks; no reclamation is safe without
live-reader and transaction lifetime rules. These are requirements for the next
experiment, not claims that row-level concurrency or retention GC is implemented.
Native providers replace historical projection; the current writer serialization
and unsupported-operation guards remain until their replacements pass tests.

## Writable binding audit checkpoint — 2026-10-22

The [DDL contract](doc/doltgres/catalog-ddl-contract.md) requires an explicit
branch/schema/storage binding at each write consumer. `dg_relation_branch` still
returns the session branch, and roots do not carry a validated decoding-schema
binding. Replacing these assumptions is prerequisite to branch-local DDL. The
current writer serialization remains. The new safety test confirms savepoint
failure preserves preceding DML without exposing it to other sessions or branches.
