# Row-level tracking and concurrent publication plan

> Current direction: see [the latest clarification](#catalog-isolation-and-native-ddl-semantics--2026-10-22).
> Earlier runtime-name/private-namespace proposals are historical and superseded
> where they conflict with that clarification.

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

### Decision: handles derived from content, allocated ones in a catalog (2026-10-28)

This supersedes the handle and key-to-handle trees of the 2026-10-25 decision
below, which stay described there for the lock-record design they share. Both
trees were rewritten on every row change, a leaf path each, and the
key-to-handle tree was keyed by digest, so every fold or batch paid for them
in chunks and WAL; and a new branch derived and wrote both for every row.

**Derived handles.** A row occurrence's handle, the TID the executor sees, is
the leading 40 bits of the row's digest whenever that is possible. Nothing
stores it: it is a pure function of the row, so the same row has the same TID
on every branch and in every revision, and a branch inherits nothing.

- *Handle to row:* a range lookup in the content tree for the digests with
  that 40-bit prefix (one descent; almost always one digest). The row whose
  occurrence count exceeds its allocated handles owns the derived one.
- *Row to handles:* the derived handle, from the digest, plus the allocated
  ones the delta holds for the digest (an in-memory probe).
- *Sequential scans* walk the content tree alone; each entry yields its
  occurrences. *Index scans* derive the handle from each entry's digest.
  *Bitmap scans* treat a lossy page, a range of 291 handles, as one digest
  range of the content tree, scanned once.

**Allocated handles** come from the per-storage counter, above 2^40, for the
occurrences that cannot take the derived handle:
- further occurrences of a row already present (duplicates in a keyless table);
- a row whose prefix another live row holds (about n²/2^41 of n rows);
- a row whose derived handle a live lock record still names, as the root or
  current handle of an update chain, or a row the command's own batch removed
  and adds back. A record's handle must not name a new row while the record
  describes the old one; a record whose row was deleted by a committed
  transaction is dead: a row that returns takes the handle and creates a new
  record, lookups prefer a live record over a dead one, and a locker holding
  the old row's TID still finds the dead record and learns of the deletion. The journal tolerates a repeated sequence for the same reason: a
  removal and the addition after it net out.

The catalog `pg_dolt_handle` maps (storage, branch, handle) to digest, with a
second index by digest. Rows are inserted when the occurrence is written and
deleted when it is removed, so ordinary MVCC gives each snapshot exactly the
allocated handles live under it, including a transaction's own, savepoints and
prepared transactions. A per-backend cache of a stream, keyed by snapshot kind
and validated like the journal's folds, serves the lookups. Revisions and
private rewrites number their duplicates for the state alone, from the top of
the TID space, as does a reader of a branch whose storage has no roots yet.

**What is no longer true below:** the handle tree, the key-to-handle tree, the
`roothandles` and `rootkeys` columns, branch creation deriving handles by
diffing, handles derived 1 to n in content order, and "IDs are never reused"
(a derived handle is the row's; it is reused when the same row returns, unless
a live record still names it). The counter still restarts above
the largest allocated handle, now read from `pg_dolt_handle` and the journal.

**Known limitation of the experiment.** Two transactions that concurrently
insert the first occurrence of byte-identical rows, with no unique constraint
to stop them, both derive the same handle. Readers then see two occurrences
with one TID until a later write allocates for one of them; nothing waits or
fails. A reservation on the derived handle, as unique keys use, would close
this.

### Decision: prolly handle trees plus heap lock records (2026-10-25)

> Superseded in part on 2026-10-28, above: the handle and key-to-handle trees
> are gone. The lock records and the isolation rules stand.

This supersedes an earlier same-day decision to store handles as heap tuples
per head. Building those for every row made branch creation O(rows), and branch
creation must be nearly free.

State is split by lifetime.

**Durable, branch-shareable, per (head, relation), all prolly trees in the
chunk store:**

- **Content tree** (canonical): row digest → occurrence count + row bytes.
  Index content trees work the same way.
- **Handle tree** (runtime, never in history): handle → row digest. The handle
  is the TID. It names a row *occurrence*, and it stays the same when the row is
  updated: an update maps it to the new version's digest. Identical rows in a
  keyless table get one handle each.
- **Key-to-handle tree** (runtime): row digest + handle → present. Index scans
  use it to turn index entries into TIDs, and sequential scans walk it in step
  with the content tree.

Branch creation copies the roots, so it is O(tables). It derives handles by
diffing from the closest existing head's (content, handle) pair, so it is
O(changes), never O(rows).

**Working roots** live in a catalog, one row per (storage resource, branch name)
holding the current roots. A snapshot selects roots from it under ordinary
MVCC. The proof, in [the chunk store plan](CHUNK-STORE-PLAN.md) milestone 4,
relies on publication holding the row lock until commit. Ref names are
authoritative: a rename rewrites the rows' branch name.

**Transient, never copied, also per (head, relation):** lock records. A catalog
row (resource, branch, handle) is created only when a row is first locked,
updated or deleted. PostgreSQL's heap tuple locking on that row provides:
- the four row-lock strengths;
- multixacts;
- waits on transaction IDs and deadlock detection;
- `NOWAIT` and `SKIP LOCKED`.

A lock on branch A never blocks branch B, because their records differ.

**Handle IDs** come from a per-resource, non-transactional counter in shared
memory. IDs are never reused. After a restart the counter starts above the
largest handle in any root row version, including prepared transactions.

**Where PostgreSQL metadata goes:**

| PostgreSQL concept | Location |
|---|---|
| TID | The handle: derived from the row's digest, or an allocated one from `pg_dolt_handle` (2026-10-28) |
| Row visibility (xmin and xmax) | Presence in the snapshot's handle root, chosen through catalog MVCC plus the transaction's own overlay |
| cmin and cmax | Command IDs on the transaction overlay |
| Row locks | The lock record's tuple header |
| "Updated" or "deleted" by a concurrent transaction | Comparing the row's digest in the snapshot root with the latest committed root, after waiting on the lock (amended below: handle presence plus the record's heir) |
| Update chains and EvalPlanQual | The same TID: the latest version is read by handle from the latest committed root (amended below: a new TID per version, found through the heir) |

**Isolation and recheck rules:**
- **Read Committed:** after waiting on a lock record, the writer reads the
  latest committed version by the same TID and re-evaluates (EvalPlanQual).
- **Repeatable Read:** if the latest version differs from the snapshot's, the
  writer raises a serialization error.

Conflicting row edits are excluded by lock records. So publication's three-way
merge only combines disjoint changes, plus additive multiset counts. Unique
indexes reserve keys so concurrent equal inserts wait instead of merging.

The acceptance gate is PostgreSQL's isolation suite run with prolly as the
default table access method.

### Implementation amendments (2026-10-24)

Building the decision above changed four details. The split between prolly
trees and heap lock records is unchanged.

- **A TID names a row version, not a row.** An update gives the new version a
  new handle, as a heap update makes a new tuple. With one TID per row, AFTER
  triggers, RETURNING and foreign key actions read the new row where they
  asked for the old one, because the executor fetches the old version by TID
  after the update.
- **Lock records mirror row versions.** Locking a row locks its record with
  `heap_lock_tuple`; updating the row `heap_update`s the record to name the new
  handle; deleting the row `heap_delete`s it. Heap code then decides every
  outcome: TM_Updated with the successor's TID, TM_Deleted, waits, NOWAIT and
  SKIP LOCKED, following update chains for SELECT FOR ..., and carrying
  key-share locks onto new versions. The record's primary key is its first
  handle, which never changes, so these updates are never key updates; a
  second index finds a record by its current handle. Records live only per
  branch and are never copied, so branch creation still copies nothing.
- **Lock records are inserted frozen.** A record inserted normally belongs to
  its uncommitted inserter, which would hold every row it first locks
  exclusively, even against compatible lockers. A frozen record is visible to
  everyone at once and is only an anchor. A short heavyweight lock stops two
  backends creating one record.
- **Unique keys are reserved by lock records** keyed by the index storage and a
  64-bit hash of the key, held exclusively until commit. A conflicting row that
  a running transaction is deleting, or updating with a key change, is waited
  for by taking a key-share lock on it, as heap unique checks wait for the
  deleter.
- **A version is current** if its record allows the operation and the latest
  committed roots, merged with the transaction's changes, still hold it. The
  second check catches history installs, which replace rows without records.

Two supporting changes:
- **Handles.** Every row's handle is derived from its content (2026-10-28,
  above), so every reader on every branch agrees on it. The shared counter
  allocates above 2^40 for the occurrences that cannot be derived, so the two
  ranges never meet.
- **Chunk puts never wait.** A put that finds no committed copy inserts its own,
  keyed by its transaction ID. Waiting on another transaction's in-progress
  copy could deadlock two publishers at commit.

Canonical storage bindings lock rows the same way, and the table AM checks
their unique indexes itself, because their writers may bypass the executor's
index insertion. Their writers share the branch resource lock, which merge
takes exclusively. Reads of canonical bindings go through versioned scans that
cannot take row locks yet, so explicit SELECT FOR ... is still rejected there,
and their foreign key checks take the branch lock exclusively instead.

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

## Schema membership publication checkpoint — 2026-10-22

Canonical branch membership is now MVCC-visible through working refs. Branch-local
DROP preserves membership and data for an older SQL snapshot, while new statements
on that branch reject the retained physical relation. Full abort, savepoint rollback
and crash-aborted DROP were tested against a running server. Canonical schema chunks
contain no PostgreSQL transaction metadata.

This does not solve concurrent writers: branch DDL uses the existing broad workspace
lock, and writes still use the current session branch. General writable bindings,
schema-generation conflict checks and three-way working-set publication remain
required. Do not extrapolate this membership-only experiment to layout-changing
ALTER or OID-only type/function caches. See [the current boundary](doc/doltgres/catalog-schema-provider.md).

## CREATE publication checkpoint — 2026-10-22

Branch-local CREATE now publishes table/index physical objects, initialized roots,
canonical schema chunks and working membership in one PostgreSQL transaction.
Staged/HEAD remain unchanged until add/commit. Initialization on a non-main branch
explicitly starts from empty content rather than importing a missing HEAD member.
Capture includes only the new table/indexes plus existing branch membership.
Rollback and crash rollback leave neither a physical table nor partial membership.

This retains broad workspace locking and Read Committed mutation; it does not
implement concurrent schema publication or generation-aware writable bindings.
See [the CREATE checkpoint](doc/doltgres/catalog-create.md).

## Writable schema-generation bindings — 2026-10-22

[The runtime binding plan](CATALOG-RUNTIME-BINDING-PLAN.md) requires DML/COPY/index
paths to carry an authoritative logical table, branch and schema generation, rather
than relying solely on the session branch or physical Relation pointer. Validate
that authority against schema publication; stale-generation writes must block,
rebind or fail explicitly. Successor-generation ALTER must retain old reader and
portal bindings while publishing schema, transformed data/indexes and mapping
atomically.

Broad serialization can remain for the first proof, with lock keys/order documented;
it does not replace the planned row-level conflict tracking and three-way working-set
publication. Retain generations initially, and specify pins/reclamation before
claiming scalable runtime OID/storage lifetime. See the new plan for same-name,
concurrent-reader, rollback and crash-recovery acceptance gates.

## Catalog isolation and native DDL semantics — 2026-10-22

Use the [current catalog execution plan](CATALOG-DDL-PLAN.md#current-execution-plan-and-ddl-semantics-clarification--2026-10-22)
and [runtime contract](CATALOG-RUNTIME-BINDING-PLAN.md#current-runtime-contract--2026-10-22)
in place of the rejected private-namespace/runtime-name workaround. Bound schema
context supplies definitions; stable storage resources supply shared lock and WAL
identity. Never substitute backend-local definition OIDs for cross-session resource
keys or infer branch authority from a physical Relation pointer alone.

The hard DDL requirement is isolation between branches. Preserve PostgreSQL's
command-specific transaction behavior, including rollback/savepoints for ordinary
table DDL; adopting Dolt's DDL transaction model is not required. Coordinate schema
and data/index publication as necessary for native consistency, without adding a
separate Dolt-style transaction requirement. This does not change the row-level
concurrency milestones or the working-set three-way publication design.

Add multi-session tests in which A changes schema while B continues reading and
writing its own definition and using valid cursor/prepared-plan bindings. Test A's
commit, abort and savepoint rollback. Same-branch stale layouts must block, rebind
or fail explicitly; another branch's schema must not be replaced or its workload
held behind a transaction-duration global DDL freeze. Document actual shared
resource contention separately from logical branch/schema conflict handling.
