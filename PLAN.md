# PostgreSQL Prolly Storage and Version Control Plan

> Current direction: see [the latest clarification](#current-catalog-direction-and-ddl-requirement-clarification--2026-10-22).
> Earlier runtime-name/private-namespace proposals are historical and superseded
> where they conflict with that clarification.

This document captures requirements and a partial plan to create a fork of Postgres with Dolt's
prolly tree storage and version control capabilities.

## Planning documents

Keep detailed subsystem plans in separate documents linked from this main plan.
The main plan owns project requirements, architectural direction and phase gates;
sub-plans own detailed protocols, milestones, acceptance tests and open choices.
Update both when a sub-plan changes the overall direction. Use this convention
for future detailed planning work.

- [Row-level tracking and concurrent publication](ROW-LEVEL-TRACKING-PLAN.md) —
  Phase 3: root-based snapshot views, out-of-band row tracking, concurrent writers,
  publication, recovery and isolation milestones.
- [Database manifest and version-control publication](MANIFEST-PLAN.md) —
  heap-backed refs, immutable database states, transaction visibility and an
  early version-control slice coordinated with the row-level publication design.
- [Chunk store](CHUNK-STORE-PLAN.md) — a database-wide store of prolly nodes
  keyed by node hash: a heap catalog with a unique btree, native access and a
  node cache. AMs keep only runtime state. Also records the packed-journal
  alternative and milestones.
- [Prolly write performance](WRITE-PERF-PLAN.md) — measured insert costs, root
  causes and a four-step plan (chunk cache, packed appends, statement-level
  deferred flush, batched inserts). Deferred until the chunk index is designed.
- [Branch relation identity and resolution](BRANCH-RELATION-IDENTITY-PLAN.md) —
  durable OIDs per (branch, table), leased OIDs for revisions, resolution of every
  prolly reference through a ref's canonical schema to an ordinary relation, and
  branch-scoped locks that replay on hot standbys. Supersedes runtime handles and
  DoltVersionedScan.

### Planning record — 2026-10-22

The plans and original DESIGN.md were copied unchanged from the workspace into
this repository and committed before this revision. These repository-root files
are now the authoritative, version-controlled plans. Earlier dated notes and
superseded proposals remain as historical evidence; later decisions take
precedence. Historical `reports/...` links originally refer to the workspace
reports directory, not a tracked directory in this repository.

- [Native catalog binding](CATALOG-BINDING-PLAN.md) records the selected provider
  direction and completed read experiment.
- [Branch-local DDL viability gate](CATALOG-DDL-PLAN.md) defines the next experiment,
  implementation sequence, risks and acceptance tests.

## Part I — Amended design and requirements

### 1. Project goals and compatibility

We have forked PostgreSQL and will replace its user-table tuple and index storage
with prolly trees. The reference implementation is Dolt, checked out at
`/workspace/dolthub/dolt`, particularly `go/store/prolly`, its `tree` package,
other packages under `go/store`, and the database/version-control integration
under `go/libraries/doltcore`.

The goal is to retain PostgreSQL features and compatibility—including its wire
format and protocol, durability, functions, types, query planning, transactions,
locking, isolation levels, and replication to other instances of this fork—while
adding:

- Version control of table data and schema together.
- Multiple branch heads in one database, editable concurrently by different
  sessions.
- Efficient differences between revisions or branches of a table.
- Branch merging through `dolt_merge()`.
- Dolt-style system tables and table functions, including `dolt_diff()` and
  `dolt_log` facilities.
- Remote clone, push, and pull operations.

Replace only as much PostgreSQL code as necessary. Preserve non-storage code where feasible so
future upstream releases remain practical to integrate.  The main implementation direction is a new
table AM and new index implementation, supplemented by the core changes needed for branch-aware
schemas, transactions, and recovery.

The following first-draft scope decisions qualify the compatibility goal:

- For user tables, `btree` denotes the prolly-tree-backed ordered index
  implementation. Other index families are unsupported in the first draft and
  must produce an explicit error when requested, including through table or
  constraint creation that requires an unsupported index AM. They must not
  silently fall back to another index type.
- Implicit SQL transaction commits by version-control commands may initially be
  restricted to standalone, top-level statements.
- Global sequence allocation intentionally differs from fully independent,
  reproducible branch state, as specified below.

These decisions do not waive PostgreSQL locking, isolation, transaction, or
constraint semantics within the supported scope. Internal/bootstrap catalog
index implementation is a separate integration decision; it is not automatically
covered by the user-table `btree` substitution.

### 2. Prolly-tree properties

The prolly tree is the central storage structure. We need not copy Dolt's exact
implementation, but must reproduce these properties:

1. **Content addressing.** Each node is addressed by a hash of its contents.
   Internal nodes identify children by their content hashes.
2. **History independence.** Under the same table and column definitions
   (including collation) and storage format, any sequences of INSERT, UPDATE,
   DELETE, and other statements that produce the same byte-identical PostgreSQL
   tuple values, NULLs, and row multiplicities must produce the same tuple-store
   representation and table content hash.
3. **Structural sharing.** Small edits rewrite affected paths and any nodes
   needed to restore deterministic chunk boundaries. Unchanged chunks remain
   shared by all roots that reference them. This is not a promise that every
   edit changes exactly one path.

This is a storage serialization contract, not a requirement to normalize every
pair of values that compares equal under SQL equality. Values pass through a
standard serialization path, including the canonicalization that path already
performs. Numerics with different scales, or distinct strings that compare equal
under a collation, may retain different serialized representations. Different
column or collation definitions are outside the fixed-schema invariant. Index
ordering, comparison, and uniqueness semantics are separate requirements.

“Tuple values” means the value contents, not physical heap headers, padding,
transaction markers, or location-dependent TOAST pointers. NULL and duplicate-row
multiplicity require explicit encodings. Format and comparator versions must be
explicit so the scope of a hash-equality guarantee is known.

**Out-of-line TOAST decision:** a value not stored inline is represented by the
content-addressed hash of its stored payload chunk. Identical serialized payload
bytes therefore have the same reference regardless of their allocation or write
history. This replaces physical TOAST identity in canonical tuple contents;
hashing does not attempt to normalize SQL-equivalent but byte-distinct values.

**Accepted custom-type limitation:** a custom type's serialization may differ
for semantically equivalent values. We accept those distinct encodings rather
than require semantic canonicalization. If its serializer is nondeterministic
for identical input value bytes, it can also break the storage history-independence
guarantee. Record this limitation and proceed with implementation; changing
PostgreSQL's type interface or solving custom-type canonicalization is outside
the current scope.

### 3. Data, indexes, and schema are versioned together

A database content root records the tables, row contents, schema definitions,
and other database objects necessary to reproduce the versioned database.
These include:

- Namespaces, including empty namespaces.
- Tables, columns, types, and index definitions.
- The actual index trees, not only their definitions.
- Triggers, functions and stored procedures.
- Foreign keys and other constraints and dependencies.
- Sequence definitions, subject to the allocator exception in section 7.
- Other database-level definitions required by supported PostgreSQL features.

A single atomic change can update both schema and data. Index contents must be
consistent with the corresponding table and schema at each published root.

**Physical index contents are stored as prolly trees and versioned alongside
table contents.** They are not merely disposable local indexes rebuilt after
every checkout or merge. Where valid, index trees should be merged directly to
avoid reconstructing the entire index from table data. Direct index merging
must still preserve key derivation, uniqueness, and row-reference correctness.

Runtime index-maintenance metadata that depends on transaction history is
excluded from canonical content hashes. This exclusion applies to auxiliary
state, not to the actual canonical index tree.

### 4. Content, runtime state, and version-control history

Separate three classes of state:

| Class                                    | Examples                                                                                                        | Hash/identity requirement                                                                        |
|                                          |                                                                                                                 |                                                                                                  |
|------------------------------------------|-----------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| Canonical database contents              | Schema objects, row values, actual index trees, canonical references                                            | History-independent within the defined storage serialization and index contracts                                |
| PostgreSQL runtime and transaction state | XIDs, command IDs, visibility metadata, locks, runtime OID mappings, row-version handles, statistics and caches | Kept outside canonical content hashes; persistent components still need recovery                 |
| Version-control history and refs         | Commits, parents, authors/messages, tags, working sets and manifest refs                                        | Content-addressed as appropriate, but intentionally capable of differing for different histories |

Two commits may have different hashes while pointing to the same database
content root. Runtime metadata must not indirectly contaminate content hashes
through references, keys, or child nodes.

“Out of band” does not mean ephemeral, unlogged, or exempt from crash consistency.
The storage and recovery requirements of each runtime structure must be defined.

### 5. Commit graph, manifest, and working sets

Implement mutable manifest metadata using PostgreSQL's existing heap, MVCC,
locking and WAL infrastructure. The [manifest plan](MANIFEST-PLAN.md) brings this
foundation and a narrow version-control slice forward from phases 4–5, before
full row-level concurrency, while designing their publication contract together.
The remaining phase gates still apply; a manifest is a logical mapping, not a
requirement for one mutable pointer or a separate file.

The manifest foundation is implemented with heap-backed object/ref catalogs,
revision-token CAS and durable canonical-tree export. See the
[manifest milestone status](MANIFEST-PLAN.md#foundation-milestone-implemented).
The [first command slice](MANIFEST-PLAN.md#first-command-slice-implemented) now
adds limited canonical schema capture, staging, commit, branch/tag creation, log
and table-level diff summary. It does not yet connect ordinary DML to working
refs or provide checkout, historical SQL binding or row-level merge publication.
Current-slice publication hardening and an external Doltgres compatibility pass
are complete; see the [verification report](reports/doltgres-compatibility.html).

Commits form a graph recording their parents, similar to Git. Merge commits
have two or more parents. A commit references the versioned database contents,
including schema, rows, and indexes.

A database-wide manifest records refs for branch heads, tags, and working sets.
Each ref points by content hash to the relevant object. Each branch head has a
working set representing durably persisted changes not yet recorded in a
version-control commit on that branch.

SQL transaction commit and version-control commit are distinct:

- SQL commit makes changes durable and publishes the affected working-set/ref
  updates atomically.
- A version-control commit creates a history node and updates the applicable
  branch head, as part of an atomic SQL transaction boundary.

A transaction that changes versioned state updates the manifest as required;
a read-only transaction need not manufacture a new manifest or history node.
One SQL transaction can atomically update N working sets or branch heads within
one database. No observer may see a partially published multi-ref transaction.
Cross-database distributed transactions are not implied by this requirement.

### 6. Sessions, branches, and SQL extensions

Each session connects to a selected branch and can change that selection with
`dolt_checkout()`. Unqualified table names resolve using the session's branch
in the analysis/planning path.

A query may explicitly reference a branch or other supported ref independently
of the session's selected branch. Required branch-qualified forms include:

```sql
SELECT * FROM "mydb/myBranch".public.myTable;
SELECT * FROM "mydb@myBranch".public.myTable;
```

A single query can reference the same logical table on different branches,
including branches with different schema definitions. A session-wide storage
switch alone cannot satisfy this requirement.

Branch/ref identity must survive name resolution and planning through execution.
The exact snapshot at which a mutable ref is read must follow the transaction's
isolation semantics; selecting a branch does not authorize reading its latest
root without regard to the active snapshot.

Statements such as `SELECT dolt_commit(...)` can implicitly commit the SQL
transaction. In the first draft, this behavior may be restricted to a standalone,
top-level statement.

### 7. OIDs and sequences

#### 7.1 Runtime OIDs and canonical object references

Built-in system OIDs retain their existing values because PostgreSQL code,
libraries, and tools depend on them.

Non-built-in runtime OIDs may be assigned persistently or allocated sequentially
**provided those allocation-dependent numbers are excluded from canonical tree
serialization and hashing**.

Canonical schema references use names or other identifiers derived in a
history-independent manner. Runtime OIDs are mapped to and from those references.
Embedded references inside expressions, types, and values must be accounted for,
not only foreign-key-like fields in catalog rows.

Rename and drop operations may consequently be more expensive. That cost is
acceptable. The exact identity/mapping strategy is an implementation choice;
a mapping must not silently use history-dependent surrogate IDs as canonical
identities.

**Current implementation policy:** until portable identifier translation exists,
reject storage of OIDs and other non-portable identifiers in canonical tuple
contents. This includes `oid`, `oidvector`, all `reg*` aliases, ACL role references,
`pg_node_tree`, transaction/command IDs, TIDs, WAL positions, snapshots and cursor
references. Reject them recursively through domains, arrays and composites,
including NULL fields and empty containers. Revalidate when nested type definitions
change. Existing values are not silently translated or rewritten.

Admit only reviewed built-in binary codecs and structural codecs that omit
local identifiers. Enums serialize labels; recursive arrays/composites omit
embedded type OIDs. Opaque custom base-type serializers and user-defined ranges
remain unsupported until their identifier portability can be established. This
is separate from the accepted semantic-equality/canonicalization limitation in
section 2; no SQL-equality normalization or PostgreSQL type-interface change is
required. Plain numeric, textual and binary data is not heuristically interpreted
as an object reference. Runtime OIDs/TIDs outside canonical contents remain valid.

#### 7.2 Global sequence allocation

Branches with a sequence of the same name and definition share a globally
consistent allocator within the database instance. Sequence definitions are
versioned; live allocator state is tracked separately.

History-independence tests involving sequence-backed columns assume literal
values are inserted rather than requiring independent executions of `nextval()`
to produce identical results. Sequence-generated values, once inserted, remain
ordinary stored values; allocator metadata must not affect their content hash.

A clone inherits sequence definitions and related stored column values, not the
source instance's live allocator state. It bootstraps its local sequence state
from that information. Dolt is a reference for this behavior. Independent clones
do not acquire a globally coordinated cross-server counter through this rule.

Bootstrap details—including increment direction, bounds, cycling, associations
with columns, and sequences with no related stored values—need an explicit
algorithm. Rename, definition changes, `RESTART`, `setval`, and drop/recreate
also require rules consistent with the shared-allocator requirement.

### 8. Transactions, WAL, replication, and reclamation

Begin with **canonical prolly trees plus separate MVCC/version metadata**.
Use Dolt-style three-way merging to reconcile working-set changes for publication,
using transaction-start metadata to identify the base. PostgreSQL locking,
conflict handling and isolation semantics govern whether and how those changes
may publish; successful tree merging alone does not establish SQL correctness.
This permits the merge mechanism without adopting Dolt's transaction semantics.
See the [publication decision](MANIFEST-PLAN.md#working-set-publication-decision).

Keep PostgreSQL WAL as the recovery authority and source of truth for crash
durability. WAL payload design may be optimized, but content/root publication
must remain coordinated with PostgreSQL transaction outcomes. A root hash alone
is not sufficient recovery information unless all referenced contents are
reliably recoverable under the same protocol.

Support both:

- PostgreSQL-style WAL replication and logical replication to compatible
  instances of this fork.
- Dolt-style clone, push, and pull of content and version-control history.

These serve different purposes and require distinct consistency protocols,
even if they share chunk transport or storage.

Garbage collection must retain content reachable from all relevant roots,
including heads, tags, working sets, active snapshots, prepared transactions,
replication retention, and in-flight remote operations. Branch heads alone are
not a sufficient retention set.

### 9. Keyless tables

Tables without primary keys, including duplicate identical rows, remain
supported. Dolt's hash-plus-cardinality representation is a reference, not a
requirement.

Choose a representation that minimizes changes to PostgreSQL planning and
execution while satisfying canonical content equality. Runtime row identity and
version identity may be stored separately. Insertion-order-dependent hidden row
IDs must not become canonical keys that make identical logical tables hash
differently.

## Part II — Implementation notes and staged plan

### 10. Architectural direction

The agreed storage direction is: implement the table-AM contract and
an ordered index-AM implementation backed by prolly trees. Reuse PostgreSQL's
parser, optimizer search, executor operators, protocol handling, transaction
coordinator, and lock/deadlock machinery where possible.

This is not an AM-only project. Branch-aware catalogs, canonical object mapping,
atomic root publication, recovery, and top-level transaction-control extensions
need targeted core integration. Keep those integration points explicit and
concentrated rather than spreading branch checks throughout unrelated code.

A proposed decomposition is:

| Component                             | Responsibility                                                                                                                         |
|                                       |                                                                                                                                        |
|---------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| Canonical encoding and prolly storage | Encode values/definitions; deterministic ordering and chunking; hash nodes; share unchanged chunks                                     |
| Table AM and slot adapter             | Scans, tuple access, modification, locking results, PostgreSQL Datum/slot conversion, maintenance and parallel access                  |
| Ordered index AM                      | Ordered lookup, uniqueness, included columns, index-only access, index build/validation, costing and supported operator-class behavior |
| Runtime version and identity state    | Row/version handles, XID/command visibility, runtime object mappings, cleanup horizons                                                 |
| Branch-aware schema integration       | Resolve refs and names, supply catalog/type/relation descriptions, handle DDL and invalidate dependent caches                          |
| WAL and transaction publication       | Recover chunks and runtime state; commit multi-ref changes; handle abort, savepoints, and two-phase outcomes                           |
| Version-control services              | Diff, merge, commit graph, working sets, system tables/functions, clone/push/pull                                                      |
| Replication and GC                    | WAL/logical replication integration; reachability and retention across all consumers                                                   |

This is a project in the postgres project written in the same programming language as postgres,
using Dolt and Doltgres source as a reference. Given the difference in language, we won't be
directly using Dolt or Doltgres libraries, but porting their logic as necessary.

### 11. Catalog strategy: evaluate deep integration, not just projection

Evaluate two primary design candidates:

1. **Branch-aware catalog access and caches.** Supply schema objects using a
   branch/revision context and adapt cache keys, invalidation, DDL, and lookup
   paths accordingly.
2. **Canonical metadata with runtime catalog projections.** Materialize or adapt
   versioned definitions into PostgreSQL execution identities, with distinct
   runtime objects where different branch definitions must coexist.

A persistent-OID mapping may be useful in either approach. An OID derived by
truncating a hash is not a complete identity scheme: collisions, lifetime,
branch distinctions, and client/cache reuse still need treatment.

The prototype must cover more than table-name lookup:

- One query joining two versions of a table with different columns and types.
- Function/operator/type lookup, defaults, constraints, and stored expressions.
- Prepared plans, portals, relcache, typcache, syscache, and shared invalidation.
- DDL rollback and concurrent DDL on different branches.
- Relation and row-lock identity that distinguishes branches without confusing
  transactions acting on the same branch.
- PostgreSQL catalog queries and stable client-visible type information.

Canonical expression serialization needs a symbolic/reference-aware format.
Raw PostgreSQL catalog or parse-tree bytes may contain runtime OIDs and are not
an acceptable general canonical schema format.

#### Decision update — 2026-10-22: native versioned catalog provider

Proceed with explicit per-reference version contexts and native metadata
providers, extending core catalog infrastructure where evidence requires it.
The persistent materializer/projection approach is withdrawn: requiring user
preparation violates the query contract, and hidden persistent reconstruction
would also violate cold read-only/standby behavior. Do not restore it as a
prerequisite or assume unchanged OID/cache infrastructure is sufficient.

The completed read milestone demonstrates automatic incompatible historical
schemas and independent branch working roots in one query, without persistent
catalog projection. It does not prove branch-local DDL or generalized versioned
types/functions. Ordinary PostgreSQL catalogs and access methods remain intact;
runtime identities may still be necessary for objects whose engine interfaces
require them, but are not portable canonical identity.

The next decision gate is [transactional branch-local DDL](CATALOG-DDL-PLAN.md),
including dependency-consistent binding and concurrent readers. The serious risks
are OID-based type/function resolution, atomic schema/data publication, cache and
prepared-plan isolation, authorization/introspection consistency, and retention.
Avoid accumulating independent lookup exceptions: establish one explicit binding
context and a documented provider boundary. Unsupported combinations must fail
explicitly, never silently bind a current object from another version.

### 12. Canonicalization, row identity, and indexes

Use the storage serialization contract in section 2 as the tree-hash correctness
oracle. Define a standard serialization path for values, NULLs, arrays/composites,
enums, and embedded object references, with explicit format versioning. Reuse
that path's canonicalization without adding SQL-equality normalization for
numeric scale, collations, or custom types. The accepted custom-type limitation
must not block implementation or require a new PostgreSQL type interface.

For out-of-line TOAST values, serialize the payload into a content-addressed
chunk and store its hash in the canonical tuple. Reads resolve the hash to the
payload; retained historical roots must retain their referenced payload chunks.
Choose deterministic inline/out-of-line and payload encoding rules so physical
TOAST placement, compression history, or allocation identifiers cannot affect
the canonical result. Chunk persistence and reference publication must participate
in recovery so a published tuple never references a missing payload. This is a
design decision for implementation, not a claim that the current adapter already
supports content-addressed TOAST.

Keep index operator-class and collation support separate from this byte encoding
contract: correct ordering and uniqueness checks still follow PostgreSQL's SQL
semantics even when equal values have distinct stored bytes.

For keyless rows, compare representations against both canonicalization and
PostgreSQL behavior. Candidate designs include a canonical multiset plus
out-of-band occurrence/version handles, or deterministic occurrence keys with
separate stable runtime handles. Neither is selected. Test individual duplicate
updates, deletes, locking, and TID-based access before committing to a format.

Keep three identities distinct:

- Canonical row/key representation used in table and index trees.
- Runtime row/version handle used for PostgreSQL execution and locking.
- Content hash identifying an immutable node or root.

A secondary index tree must not serialize a history-dependent runtime TID as
its canonical row reference. An adapter must connect canonical references to
runtime tuple/version access. Handle retention must account for concurrent
snapshots and outstanding references, not just the current command.

The first ordered index implementation must preserve the supported B-tree SQL
contract: ordering, NULL placement, uniqueness semantics, expression and partial
indexes, included columns, and supported operator-class comparisons. Capability
flags and cost/statistics callbacks integrate it with the existing planner;
misdeclaring capabilities is not an acceptable shortcut.

For merges, use direct table/index tree merging where the schema, canonical
keys, and merge outcome make it valid. If table conflict resolution changes
rows, reconcile the corresponding index entries. Rebuilding affected portions
is a fallback, not the default architecture or a substitute for versioning
actual index trees.

### 13. MVCC, locking, and isolation integration

Retain PostgreSQL's transaction coordinator and shared lock/deadlock framework.
Implement the new AM's visibility and mutation semantics against separate
version metadata; heap visibility routines do not automatically apply because
they interpret heap-specific fields.

The contract must address:

- Statement snapshots for READ COMMITTED and retained snapshots for stronger
  isolation levels, including branch/ref-root visibility.
- Own-write and command-counter visibility; subtransaction and savepoint state.
- Row-lock strengths, compatible lockers, waits, NOWAIT, and SKIP LOCKED.
- Concurrent update/delete rechecks and the table AM's `TM_Result` protocol.
- Unique checks, speculative insertion/ON CONFLICT, foreign keys, and deferred
  constraints.
- Serializable read/write dependency tracking, including empty-range reads.
- Prepared transactions and lock/state restoration after restart.
- Unified behavior while ordinary PostgreSQL catalogs or other retained storage
  coexist with the new table AM.

Separate live SQL conflict handling from explicit version-control merging.

The agreed first-pass direction is to retain root-based snapshot views and add
stable runtime row identities, version tracking, row locks and transaction-local
changes outside canonical hashes. Replace the transaction-duration table writer
lock with row-level coordination and brief publication synchronization. Apply
validated changes to the latest publication base, preserving unrelated commits;
do not publish an unconditional replacement of the starting snapshot's root.
Dolt-style three-way merge is the agreed reconciliation mechanism: merge the
transaction's base, its computed changes and the latest working set, subject to
PostgreSQL conflict checks, snapshot rules and atomic publication validation.

The detailed protocol, snapshot correctness requirements and acceptance gates
live in [ROW-LEVEL-TRACKING-PLAN.md](ROW-LEVEL-TRACKING-PLAN.md). In particular,
root selection must account for visibility of all incorporated transactions,
not just the XID attached to the root. The publication algorithm remains to be
specified and validated before replacing the current locking scheme.

### 14. WAL and atomic publication protocol

The following is a proposed protocol outline to refine, not a complete locking
or recovery algorithm:

1. Build new immutable chunks and tentative table, index, and schema roots.
2. Record enough information in WAL to recover required chunks and persistent
   runtime metadata. An initial design can favor complete new-chunk payloads
   over maximum compression or minimum write amplification.
3. Associate the complete multi-ref update with the SQL transaction. Tentative
   updates are not published as committed state.
4. Establish the PostgreSQL transaction outcome through the WAL-controlled
   commit path, respecting synchronous/asynchronous durability semantics.
5. Make committed ref changes visible atomically and ensure recovery can finish
   publication idempotently if the process crashes at any boundary.

Publication must obey snapshots as well as crash recovery. A single mutable
manifest pointer is not by itself a visibility model for older snapshots.
Concurrent publication needs a defined serialization/retry policy that does
not accidentally substitute whole-root conflicts for PostgreSQL row semantics.

### 15. Implementation phases and completion gates

These phases organize work; they are not promises of feature completeness at
every intermediate milestone. The catalog investigation in Phase 1 should run
early rather than wait for the entire storage engine.

#### Phase 0 — Specify contracts and canonical formats
 
- Define content-root equality, encoding/comparator versioning, symbolic object
  references, OID mapping guarantees, and the versioned/global state boundary.
- Select initial row/version-handle and keyless candidates for experimentation.
- Specify branch snapshot semantics and a first WAL/publication state machine.
- Establish a compatibility matrix including the explicit unsupported-index
  behavior and top-level implicit-commit restriction.

**Gate:** written invariants and falsifiable tests for history independence,
identity, transaction visibility, and crash outcomes. Remaining choices are
listed explicitly rather than hidden in placeholder formats.

#### Phase 1 — Prove the two highest-risk foundations

**Canonical storage prototype:** deterministic encoding, chunking, updates,
structural sharing, and table/index roots. Compare incremental construction
against fresh construction across varied edit histories and duplicate rows.

**Branch-schema prototype:** evaluate catalog candidates with a single query
using two divergent branch definitions, then prepared statements and DDL
rollback. Record the touched core interfaces and invalidation rules.

**Gate:** select the catalog strategy based on demonstrated correctness and
integration cost; prove that runtime identifiers do not enter content hashes.

#### Phase 2 — Single-branch table and ordered index AMs

**Native packaging:** the prolly runtime lives in
`src/backend/access/dolt_prolly`, with built-in AMs, functions and operator
classes initialized by initdb. The independent C library remains in
`src/doltgres/prolly`; SQL/TAP coverage lives in `src/test/modules/test_prolly`.
No customer extension installation is required. PostgreSQL-defined AMs and
operator classes remain available, and heap remains the default; replacing
upstream methods or changing defaults is deferred to a later, separately
specified stage. Existing routing of indexes on explicitly selected prolly
tables remains local to those tables. Native catalog changes require a fresh
cluster; no in-place prototype cluster migration is claimed.

- Implement slots, sequential/ordered scans, insert/update/delete, row fetch,
  index maintenance, and the initial out-of-band version representation.
- Support SQL `btree` through the prolly AM for user tables and reject unsupported
  index families explicitly.
- Wire in WAL/recovery and initial atomic working-set publication.
- Ordinary catalogs can remain during this intermediate phase, without treating
  that arrangement as the final branch-schema solution.

**Gate:** supported SQL operations return correct results through PostgreSQL's
normal planner/executor, roots remain canonical, and recovery restores committed
changes without publishing aborted changes.

#### Phase 3 — PostgreSQL transaction behavior

Follow the [row-level tracking sub-plan](ROW-LEVEL-TRACKING-PLAN.md) in order:

1. Specify row identity, version-chain lifetime and the snapshot/publication protocol.
2. Implement private changes and concurrent disjoint-row writers without lost updates.
3. Implement same-row conflicts, version following, executor rechecks, row-lock
   modes and savepoints.
4. Implement concurrent unique-key enforcement and atomic table/index publication.
5. Validate recovery and prepared transactions, then complete Serializable/SSI.

These are intermediate milestones, not a reduction of the Phase 3 scope below.
The table-wide transaction-duration writer lock is temporary scaffolding, not an
acceptable production concurrency design.

- Complete concurrent writer/reader, unique conflict, foreign-key, row-lock,
  savepoint, and isolation-level integration.
- Integrate SSI, command counters, cancellation, deadlocks, and prepared
  transactions with persistent runtime metadata and publication.
- Exercise mixed new-storage/catalog operations and transactional DDL boundaries.

**Gate:** targeted concurrency tests demonstrate PostgreSQL-compatible outcomes;
crash injection covers chunk persistence, transaction outcome, and publication.
Prepared transactions survive restart and resolve correctly.

#### Phase 4 — Branch-aware schema and atomic multi-branch transactions

- Integrate the chosen catalog/OID strategy beyond the early prototype.
- Implement session checkout and explicit branch-qualified resolution.
- Version schema, rows, and actual index trees in the same root.
- Atomically publish multi-working-set/head transactions with snapshot-correct
  cross-branch reads and branch-aware cache invalidation.

**Gate:** different schemas coexist in one query; concurrent branch edits work;
DDL rollback is correct; recovery and readers never observe partial multi-ref
publication.

##### Next gate update — 2026-10-22

Begin the branch-local DDL portion now, coordinated with the unfinished Phase 3
publication work. Start with branch-local CREATE/ALTER/DROP TABLE and transactional
rollback, then test dependencies and incompatible branch-local types/functions.
The read milestone alone does not satisfy this phase's gate. Keep the existing
DDL guard for unsupported paths and remove it only for operations with validated
branch isolation, publication and recovery. Schema merges and general multi-branch
writes are not implied by this first DDL milestone. Detailed stages and decisive
cross-branch/concurrent-DDL tests are in [CATALOG-DDL-PLAN.md](CATALOG-DDL-PLAN.md).

#### Phase 5 — Version-control behavior

- Implement commit graph, tags, working sets, diff/log facilities, and merge.
- Merge table and index trees directly where correct; validate/reconcile indexes
  after row or schema conflict resolution.
- Implement the restricted top-level implicit SQL commit behavior.
- Define and implement sequence allocator sharing and clone bootstrap rules.

**Gate:** independent histories with identical contents under section 2’s storage
serialization contract yield equal content roots;
merge results satisfy constraints; version-control commands respect their SQL
transaction boundary; sequence exceptions match the documented contract.

#### Phase 6 — Replication, remotes, GC, and compatibility expansion

- Complete WAL streaming/standby behavior, logical decoding/replication, and
  branch-routing policies.
- Implement clone/push/pull and atomic remote ref-update semantics.
- Implement reachability-based GC with snapshot, prepared-transaction,
  replication, and remote-operation pins.
- Expand type/operator-class, DDL, maintenance, parallel execution, backup,
  tooling, and supported extension coverage. Profile and tune WAL/chunk costs.

**Gate:** recovery, replication, remote operations, and GC preserve all retained
roots and supported SQL behavior under failures and concurrent activity.
Unsupported capabilities remain explicit, with no silent fallback.

### 16. Decisions still to make

The following remain open; none reverses the agreed direction:

1. Catalog strategy and exact branch/revision context carried through caches and
   plans, including behavior of checkout with existing prepared plans/portals.
2. Canonical object references, rename/drop rewrites, OID mapping lifetime and
   remapping during clone/restore.
3. Row occurrence/version handles, TID adaptation, and keyless encoding.
4. Standard serialization paths and format upgrades, deterministic inline and
   out-of-line payload encoding rules, and index operator-class/collation coverage.
   Out-of-line content hashes and the accepted custom-type serialization limitation
   are settled in section 2; SQL-equality normalization is not an open prerequisite.
5. Publication visibility, concurrent manifest updates, WAL record shapes,
   checkpoint recovery, and two-phase integration.
6. Sequence identity transitions and bootstrap algorithm beyond ordinary
   ascending, column-associated sequences.
7. Branch routing and semantics for logical replication and replicated
   version-control operations.
8. Schema/row merge conflicts, conflict representation, and which index changes
   can be merged directly versus reconciled from affected rows.
9. Detailed coverage for global PostgreSQL objects, roles/privileges,
   database-level definitions, extensions, and environment dependencies needed
   to reproduce a versioned database.
10. Runtime language/library integration and boundaries for reusing Dolt code.

Resolve these with small prototypes and explicit contracts. Keep core patches
organized by branch resolution, catalog identity, transaction publication, and
recovery so upstream integration costs remain visible.

### 17. Reference material

- [PostgreSQL architecture report](reports/postgres-architecture.html), especially
  option C, table/index AM contracts, catalog integration, and concurrency.
- [Transaction lifecycle report](reports/postgres-transaction-lifecycle.html),
  including WAL structure, commit ordering, locks, and recovery.
- Dolt: `go/store/prolly/tree/` for node storage/chunking;
  `go/store/val/keyless_tuple.go` for a keyless reference;
  `go/libraries/doltcore/doltdb/` for roots and working sets;
  `go/libraries/doltcore/sqle/dsess/transactions.go` for atomic head updates.
- PostgreSQL: `src/include/access/tableam.h`, `src/include/access/amapi.h`,
  `src/backend/catalog/`, `src/backend/utils/cache/`,
  `src/backend/access/transam/`, and `src/backend/storage/lmgr/`.

This is a design and implementation plan. It does not claim that the proposed
storage formats, publication protocol, or compatibility coverage have already
been implemented or validated experimentally.


## Branch and merge data milestone — October 21, 2026

The native branch-and-merge slice now implements independent session data views,
checkout, merge-base discovery, fast-forward/no-op and unchanged-schema three-way
merges. Conflicts return status flags and preserve heap/WAL-backed evidence and
pending parents for resolution or abort. Schema merges remain excluded.
(Since 2026-10-26, schemas merge object by object; see
[versioned schema objects](CATALOG-DDL-PLAN.md#versioned-schema-objects--2026-10-26).)
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

## Retained catalog binding experiment

See [CATALOG-BINDING-PLAN.md](CATALOG-BINDING-PLAN.md) for the updated catalog
architecture decision and implementation gates, and the
[validation report](reports/catalog-binding.html). Native retained schema/data
projections now support cross-revision queries; writable branch catalogs and the
shared-schema DDL freeze remain outstanding. The hybrid requires explicit core
binding/lifecycle services, not only name rewriting.

### Superseding catalog direction: automatic cold reads

The explicit materializer/projection-catalog approach is withdrawn. Versioned
queries must work without preparation, including cold read-only and standby
access. The replacement native catalog-provider experiment and acceptance tests
are specified in [CATALOG-BINDING-PLAN.md](CATALOG-BINDING-PLAN.md). Prior reports
remain historical records, not instructions to restore the materializer.

### Native catalog provider milestone (2026-10-21)

The explicit/persistent materializer was removed. Automatic in-memory versioned
bindings and lazy retained scans pass cold read-only/restart/standby tests and the
shared Doltgres revision-query slice, including incompatible schemas, historical
cursors and two branch working sets in one query. Continue with the native provider
architecture; branch-local DDL and generalized versioned objects remain the next
catalog gates. Details: [catalog plan](CATALOG-BINDING-PLAN.md) and
[milestone report](reports/versioned-catalog.html). Earlier projection proposals
are superseded, not optional prerequisites for query correctness.

## Catalog DDL audit checkpoint — 2026-10-22

Step 1 of the [DDL gate](CATALOG-DDL-PLAN.md) has a source-level contract and
executable baseline. The [audit](doc/doltgres/catalog-ddl-contract.md) demonstrates
why SQL catalog snapshots alone do not version relcache/type/function lookup.
Continue with explicit writable branch/schema bindings and transaction-local
schema deltas before lifting DDL guards. Branch-local DDL and versioned routines
remain unimplemented; the baseline Doltgres failures are documented rather than
reclassified as passes.

## Canonical schema provider implementation — 2026-10-22

Option 1 is the selected direction: immutable canonical schema descriptions live
in prolly trees alongside retained data, and native providers resolve a bound
branch/revision. Shared physical catalogs are a temporary runtime bridge, not the
canonical cross-branch definition store. No materializer or explicit preparation
is required for reads.

The first writable subset is implemented: branch-local `DROP TABLE ... RESTRICT`
updates only canonical working-set membership, atomically under PostgreSQL MVCC
and WAL. Other branches and historical reads retain their table/index storage.
Table and namespace descriptions now use the existing prolly encoding/chunk
retention machinery. See [implementation details](doc/doltgres/catalog-schema-provider.md).

This is a partial implementation gate, not general branch-aware catalogs. Capture
still obtains definitions from shared catalogs; CREATE, layout-changing ALTER,
versioned type/function identities, dependencies and logical catalog introspection
remain to be implemented. Physical catalog scans still expose retained dropped
objects. The next gate must address these limitations explicitly; removing the
remaining guards would be incorrect. Existing table-wide writer serialization
also remains. No fundamental obstacle to option 1 was found in this subset.

## Branch-local CREATE checkpoint — 2026-10-22

Native CREATE TABLE now publishes a new table and its NOT NULL / immediate PRIMARY
KEY / UNIQUE constraints into only the selected branch's canonical working set.
Physical table/index creation runs under the caller's privileges; canonical schema,
empty roots and ref publication share the SQL transaction. Non-main branches do
not require a preexisting HEAD entry for a newly created relation. Abort, savepoint
rollback, crash rollback and native index-build failure are tested.

This advances the option 1 implementation without resolving runtime identity:
a qualified physical name already retained on another branch cannot be reused.
CREATE after activation chooses the prolly AM when omitted and rejects explicit
non-prolly AMs. General CREATE forms, event triggers, deferred constraints and
layout-changing ALTER remain guarded. Raw catalog introspection still exposes the
physical bridge. The next boundary is an explicit mapping from logical identity
and schema generation to writable runtime relations; same-name divergent CREATE
and layout-changing ALTER must be tested through it rather than mutating shared
descriptors. See [the CREATE checkpoint](doc/doltgres/catalog-create.md).

## Next implementation order — runtime catalog bindings, 2026-10-22

The restricted CREATE milestone confirms that shared physical names now limit
versioned DDL. Follow [CATALOG-RUNTIME-BINDING-PLAN.md](CATALOG-RUNTIME-BINDING-PLAN.md)
and the revised current sequence in [CATALOG-DDL-PLAN.md](CATALOG-DDL-PLAN.md):

1. Logical-to-runtime mapping and authoritative per-reference bindings, proving
   same-name CREATE with incompatible schemas and independent writes on branches.
2. Successor-generation ADD/DROP COLUMN with atomic schema/data/index publication
   and stable old readers, rather than mutation of shared descriptors.
3. Logical catalog interfaces, caches, whole-row identities and versioned type/
   function dependencies, with necessary prerequisites integrated into steps 1–2.
4. Review complete acceptance evidence before expanding DDL and concurrency.

This retains option 1, canonical schemas alongside data in prolly storage. Physical
OIDs/internal names remain local, and runtime resources are not allocated per
commit. Historical reads stay automatic, read-only-capable and independent of
persistent query-time catalog preparation. These are planned changes, not claims
that the remaining mapping or generalized catalog work is already implemented.

## Current catalog direction and DDL requirement clarification — 2026-10-22

The private-namespace approach was rejected and rolled back. Earlier instructions
to allocate distinct internal catalog names as the solution to same-name CREATE
are superseded. Option 1 remains the direction: canonical versioned schemas are
authoritative, and every versioned reference carries context through native
catalog/cache/type/dependency interfaces and planning/execution. SQL introspection
must agree; unsupported paths reject rather than silently use physical definitions.

The hard DDL requirement is branch isolation: sessions on another branch retain
their definitions and usable bindings while schema changes occur elsewhere. Follow
PostgreSQL's native transaction rules for each DDL command; adopting Dolt's DDL
transaction semantics is not required. Ordinary PostgreSQL table DDL can share a
SQL transaction with DML, so preserve native commit/abort/savepoint behavior where
supported. Atomic internal publication remains necessary for coherent schema,
data/index and dependency state; it does not impose Dolt-style transaction behavior.

The [current catalog execution plan](CATALOG-DDL-PLAN.md#current-execution-plan-and-ddl-semantics-clarification--2026-10-22)
owns the implementation order and gates: canonical provider, automatic per-reference
binding and introspection, storage-resource binding, native DDL provider state,
then lifecycle/compatibility closure. Test a narrow CREATE/rollback path early.
The first integrated read gate is two incompatible canonical revisions in one
query with correct row types and catalog introspection; the writable gate includes
same-name CREATE and multi-session branch isolation during/after DDL. No manual
preparation or persisted schema projection is permitted.

The native catalog-view infrastructure is on an unmerged feature branch; its
fixtures do not establish production catalog/DDL completeness. The preserved
same-name Doltgres suite still fails. See also the updated
[runtime identity contract](CATALOG-RUNTIME-BINDING-PLAN.md#current-runtime-contract--2026-10-22).

### Branch-aware catalog completion direction (2026-10-23)

The five catalog milestones through 3717fc07817 establish execution context,
canonical routines, native dependency traversal, a native tuple proposal boundary,
and decoded-view reclamation/recovery validation. They are architectural gates,
not complete PostgreSQL catalog or DDL coverage. See
[CATALOG-DDL-PLAN.md](CATALOG-DDL-PLAN.md) and the implementation notes under
`doc/doltgres/catalog-*.md` for the bounded coverage and remaining restrictions.

Remaining work is: (1) codecs and provider integration for additional schema
objects and stored expressions; (2) general provisional catalog mutation state for
native multi-pass DDL; (3) dependency/permission coverage including shared objects;
(4) consumer audits and broader compatibility tests; (5) parallel context transport,
further session-memory reclamation and persistent garbage collection; (6) prolonged
concurrency, cancellation, savepoint and recovery tests, including investigation
of the isolated nested-DDL timeout recorded in the lifecycle report. Row-level data
write concurrency remains a related, separate storage workstream.

Prioritize provisional catalog state next. Reuse native DDL validation and ordering
against branch-local tuples, with command-visible intermediate definitions and
one atomic schema/data/dependency publication. Do not extend this by accumulating
independent SQL-command rewrites. User authorization includes broad core changes
and broad native/Doltgres tests; unsupported paths must remain explicit rather
than silently accessing a physical user definition.

### Provisional catalog implementation checkpoint (2026-10-23)

The provisional mutation infrastructure is implemented: native catalog tuple
insert/update/delete staging, command-counter visibility, local cache refresh,
savepoint rollback and immutable-reader isolation. Native multi-pass ALTER now
uses this infrastructure, including column layout changes, scalar defaults/CHECKs
and non-indexed ALTER TYPE USING. An optional table-AM rewrite callback lets the
native executor produce private replacement rows without physical heap swaps.
Canonical schema and matching data/index roots publish in the same PostgreSQL
transaction. Independent branches can rewrite concurrently.

This completes the common staging/rewrite mechanism, not all catalog codecs or
DDL operations. The next integration boundary is index lifecycle publication
(creation/deletion/replacement), followed by foreign keys, sequences/identity,
generated expressions and additional schema object families. Existing CREATE/DROP
adapters remain; they have not all been replaced by the provisional controller.
Shared-role lifetime, worker transport, handle reclamation, history GC and row-level
writer concurrency remain as listed above. See
[implementation and limitations](doc/doltgres/catalog-native-mutation.md).

### Dolt function integration decisions (2026-10-23)

Follow func-review.txt: restore canonical schemas/dependencies with matching
data/index roots; branch creation must replace working state, and deletion must
invalidate it. Do not introduce branch incarnation identifiers for this repair.
User SQL in helpers must re-enter branch-aware binding. Reserve relation and
namespace names beginning with `dolt` for internal use. Documents, stored tests
and saved queries belong in ordinary versioned prolly tables, not pg_catalog
heap tables. Fix native catalog sanity invariants. Explicitly deferred defects
are tracked in [KNOWN-ISSUES.md](KNOWN-ISSUES.md). Active-session deletion/write
coordination may be deferred if necessary, but branch recreation must not retain
old working roots.

Integration implementation: canonical restore now publishes definitions and
matching data/index roots together; hard reset restores dropped objects and
whole-database dependencies. Branch create/copy overwrite working state and
install fresh starting roots while reusing storage resources. This is currently
O(tables + indexes), with chunk import, rather than manifest-only publication.
Session locks deny ordinary deletion of an attached branch; forced-deletion
old-snapshot/recreation authority remains deferred in KNOWN-ISSUES.md.
Docs, stored tests and saved queries are public prolly tables reached through
built-in aliases. The six administrative heap tables now use native catalog
headers, C-collated text indexes and valid pg_proc argument metadata.

### Storage write performance and chunk indexing (2026-10-24)

Loading the PostgreSQL regression suite's setup data into prolly tables takes
about an hour, because every insert re-reads the relation's entire chunk history.
Measurements, root causes and the agreed four-step fix are in
[WRITE-PERF-PLAN.md](WRITE-PERF-PLAN.md). That work is paused. Its first step, a
chunk cache, depends on a decision made since: chunks must be locatable by hash,
across all relations and history, through an index serialized to disk, with no
startup scan and with a cache shared across sessions. Chunk storage and indexing
(branch `zachmu/chunk-index`, [CHUNK-STORE-PLAN.md](CHUNK-STORE-PLAN.md)) is
the next focus. The selected design is a heap catalog `pg_dolt_chunk` with a
unique btree on the node hash, with data inline and never TOASTed. It uses direct
heap and index access and a per-backend node cache. A custom packed journal with
sorted index runs is the recorded alternative. Microbenchmarks (2026-10-25)
were good enough to build on provisionally, provided every consumer uses a
standard store interface that allows replacing the implementation. The
statement-level deferred flush follows AM integration directly. It will also address the
redundancies found in the `pg_dolt*` catalog audit: chunks copied into
`pg_dolt_object`, two hash namespaces, and three diverging records of a branch's
working state.
