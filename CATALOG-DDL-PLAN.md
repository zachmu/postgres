# Branch-local DDL viability gate

> Current direction: see [the latest clarification](#current-execution-plan-and-ddl-semantics-clarification--2026-10-22).
> Earlier runtime-name/private-namespace proposals are historical and superseded
> where they conflict with that clarification.

## Decision and scope — 2026-10-22

Continue the native versioned catalog-provider direction documented in
[PLAN.md](PLAN.md) and [CATALOG-BINDING-PLAN.md](CATALOG-BINDING-PLAN.md).
Automatic table-read binding is demonstrated; generalized branch-aware catalogs
are not. This gate establishes whether branch-local schema evolution can integrate
correctly with PostgreSQL execution, dependencies, transactions and caches.

The persistent materializer was removed. No user preparation or hidden persistent
historical catalog reconstruction may be required for a valid query. Preserve
ordinary PostgreSQL access methods. Do not assume that type/function identities
can be handled by table descriptors alone or by unchanged cache keys.

## Required invariants

- CREATE, ALTER and DROP affect only the selected writable branch. Other branches
  and retained revisions preserve their definitions and readable data.
- Each table reference has an explicit binding context. Types, functions,
  operators, defaults, constraints and index expressions resolve consistently
  with that context; a statement can contain more than one context.
- Schema, dependent metadata and table/index changes publish atomically under SQL
  transaction visibility. Own changes, savepoint rollback and full abort have
  defined behavior. Commit failure cannot expose half a schema/data change.
- Existing snapshots and portals keep valid bindings. Prepared plans revalidate
  against the applicable branch/snapshot; incompatible result-type changes follow
  PostgreSQL's explicit error/reprepare rules rather than returning wrong types.
- Permissions and catalog introspection agree with the selected object identity.
  A runtime OID is not canonical identity and must not leak into portable hashes.
- Concurrent DDL/DML validates schema identity and dependencies. Version contexts
  must not be implemented by mutating one global or session-wide catalog view.
- Active bindings retain required metadata/chunks. Initially retaining objects
  without reclaiming them is acceptable; adding GC requires explicit pins.

## Current next steps — runtime binding revision, 2026-10-22

The audit and restricted DROP/CREATE checkpoints below are implemented. Their
historical descriptions are retained. The remaining implementation order is now
specified by [Versioned catalog runtime bindings](CATALOG-RUNTIME-BINDING-PLAN.md),
which refines the original sequence and takes precedence for the next work:

1. **Mapping and same-name CREATE.** Separate logical names/object identity and
   canonical schemas from local physical relation names/OIDs. Resolve each read
   and write target through a branch/schema binding; route DML, COPY, indexes,
   capture, DROP and checkout through it. Allocate runtime resources during DDL
   and publish them atomically with canonical membership. Prove independent
   same-name tables with incompatible definitions on two branches and a query
   spanning both. This is the immediate milestone.
2. **Successor generations and ALTER.** Add bounded ADD/DROP COLUMN with required
   data/index transformation. Publish a successor binding without mutating another
   branch's or active reader's descriptor. Prove rollback/recovery and explicit
   stale-write outcomes before expanding operation coverage.
3. **Logical catalogs and dependency closure.** Complete supported introspection,
   OID/name conversions, caches, whole-row/type identities and versioned function/
   type dependencies. Implement prerequisites in the earlier milestones wherever
   necessary for correctness. Do not treat a distinct relation OID as a complete
   solution for all catalog caches.
4. **Review the full gate.** Validate against matching Doltgres tests and native
   multi-session/recovery tests, then decide how far to expand DDL, concurrency and
   retention. General catalog viability requires more than the same-name test.

There will be no permanent physical catalog copy/OID per commit and no historical
query-time materializer. Historical reads continue using canonical descriptors;
local mappings support writable runtime generations. Logical identity encoding,
reuse/reclamation, mapping persistence and catalog-query context need explicit
choices during implementation, not accidental assumptions. Preserve history
independence: physical names and allocation IDs must not enter canonical hashes.

## Original implementation sequence (planning record)

### 1. Audit and publication contract

Map CREATE/ALTER/DROP paths, table/index lifecycle, row types, dependency recording,
relcache/syscache/typcache invalidation, prepared plans and utility locks. Identify
which calls require physical OIDs and which can consume versioned descriptors.
Inventory the matching Doltgres tests before changing expectations or selecting
subsets. Document the minimal provider APIs and the schema/data publication state
machine alongside [MANIFEST-PLAN.md](MANIFEST-PLAN.md) and
[ROW-LEVEL-TRACKING-PLAN.md](ROW-LEVEL-TRACKING-PLAN.md).

Choose runtime identity/mapping and cache scope based on this audit. Native
catalog integration is the selected direction; specific OID mappings, lock keys
and cache representations are not yet settled. Report surprises and consequential
choices before expanding the implementation around an unproven assumption.

### 2. Branch-local table DDL

Implement a clearly bounded CREATE TABLE, ALTER TABLE and DROP TABLE subset using
built-in column types. Exercise branch-only table existence and two incompatible
column definitions. Writes must use the branch's definition, and schema plus data
must be retained together. Include supported indexes/constraints in the contract;
keep explicit guards on unsupported operations. Do not remove the database-wide
freeze indiscriminately or silently route unsupported DDL to shared live catalogs.

Test transactional visibility, own DDL followed by DML, savepoints/full rollback,
prepared plans, long-lived readers and restart/recovery before extending coverage.
This is an intermediate proof, not completion of the generalized catalog gate.

### 3. Dependencies and versioned types/functions

Extend the experiment to at least one user-defined type and one function whose
definitions differ between branches. Determine how function/type identity,
expression binding, wire-visible type information and stored dependencies work.
Exercise defaults or constraints referring to versioned functions. Avoid encoding
raw runtime OIDs or raw PostgreSQL parse trees as canonical definitions; adding a
custom-type test requires a supported portable representation, not relaxing the
existing rejection of nonportable identifiers.

A decisive test joins incompatible branch-local table definitions in one query,
with branch-specific type/function dependencies, while another session changes
one branch's schema. It must bind each dependency to the intended version, retain
old reader behavior and expose the new definition only under the documented
snapshot rules. Record any required SQL qualification or binding rules explicitly.

### 4. Review evidence and select the next implementation boundary

Run matching existing Doltgres scripts through the external harness, plus native
multi-session, rollback, cache and recovery tests. Keep unsupported functionality
and intentional deviations separate from passing coverage. Do not weaken expected
schema/version semantics to accommodate the implementation. Report exact selected
suites, edits/adaptations, failures, and remaining untested paths.

If success requires widespread unscoped lookup exceptions, cannot isolate
simultaneous versions, or depends on persistent query-time catalog preparation,
revisit the provider/cache design before building further features on it. If the
invariants pass with a coherent provider boundary, expand operation coverage and
remove guards incrementally. Existing table reads alone cannot establish success.

## Acceptance matrix

| Area | Required evidence |
| --- | --- |
| Branch isolation | Create a table on A only; alter/drop it on A without changing B; join incompatible A/B definitions in one statement. |
| Transaction visibility | Own DDL/DML visible; another session cannot see uncommitted changes; commit/abort/savepoint outcomes coherent. |
| Concurrent execution | Old snapshot and open cursor survive DDL; same-branch conflicting DDL/DML has explicit outcomes; unrelated branch changes do not overwrite each other. |
| Dependencies | Branch-specific custom type/function bindings coexist; defaults/constraints use the intended version; DROP dependency behavior is scoped correctly. |
| Caches and plans | Prepared statements, checkout/ref changes, DISCARD PLANS and cold execution preserve version selection; incompatible outputs fail explicitly. |
| Authorization and introspection | Ordinary-reader and unauthorized cases; retained/live permission rules; supported pg_catalog/information_schema results reflect the selected branch. |
| Durability and retention | Restart/crash at publication boundaries exposes complete old or new state; retained reads survive source DROP/rewrite; no preparation after restart or on standby. |
| Compatibility | Relevant Doltgres revision/schema tests run against the fork, with exact coverage and any differences documented. |

## Non-goals and unresolved questions

Schema merge, general multi-branch writes, full PostgreSQL DDL coverage, optimized
historical index access and removal of all writer serialization are not implied by
this gate. Existing guards remain for unsupported cases. Function lookup syntax,
versioned view ownership, catalog introspection context, runtime OID lifetimes and
cache invalidation scope need explicit decisions during the audit. Keep these
open questions visible rather than treating prototype choices as final contracts.

## Step 1 audit checkpoint — 2026-10-22

The [source audit and publication contract](doc/doltgres/catalog-ddl-contract.md)
records implementation cut points and a live-server finding: SQL catalog scans
and parser/relcache lookup can observe different schema generations within one
REPEATABLE READ transaction. Native version context must cover catalog lookup;
ordinary catalog MVCC and a selected session branch are insufficient.

The first implementation prerequisite is explicit writable branch/schema binding
and staged membership/dependency changes, coordinated with data roots. The current
capture path still enumerates shared live catalogs, and the AM has a second ALTER
guard independent of the branch activation guard. Both remain intact. No branch
DDL was enabled by this audit checkpoint. The runtime OID bridge and introspection
strategy are still open choices, not validated interfaces.

Two original Doltgres revision-introspection scripts and its branch-function
scenario pass on Doltgres and fail at the current fork restrictions. A new native
27-assertion safety/visibility test passes. These are baseline evidence, not
completion of steps 2–3. See the contract for exact coverage and limitations.

## First writable subset — 2026-10-22

Branch-local `DROP TABLE ... RESTRICT` now removes table/index membership from the
selected working database descriptor. It leaves physical catalog rows and storage
in place for other branches/history. Abort, savepoint rollback and crash rollback
restore membership through ordinary PostgreSQL transaction outcome. Staged state
and HEAD move only through the existing add/commit operations. Checkout tolerates
extra retained physical objects while requiring matching definitions for target
members. Schema merge remains excluded.

This deliberately proves removal before creation/rewrite: it avoids mutating a
shared TupleDesc while testing canonical publication and reader stability. It
does not complete the explicit write-binding interface, scoped dependency graph,
or CREATE/ALTER gates. DROP CASCADE remains rejected. Ordinary capture now filters
membership through the working descriptor, but definitions still come from shared
catalogs. Raw pg_catalog and information_schema remain an acknowledged gap.

Native tests cover 15 schema-tree and 28 branch-DROP assertions. A new shared
Doltgres branch-DROP scenario passes all 17 assertions on both engines. The prior
safety test now has 25 assertions because DROP rejection moved to positive tests.
See [details and next experiments](doc/doltgres/catalog-schema-provider.md).

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

## Current execution plan and DDL semantics clarification — 2026-10-22

This decision supersedes the earlier runtime-name/mapping-first sequence and the
withdrawn private-namespace implementation. Preserve those sections as history,
not as instructions to allocate persisted catalog definitions per branch. Follow
option 1: canonical schemas live alongside table data in prolly storage and are
vended through branch-aware native catalog interfaces.

### DDL contract: branch isolation, PostgreSQL transaction rules

The hard requirement is branch isolation: a schema change on branch A must not
change the definitions, name resolution, dependencies, permissions, rows, indexes,
or valid execution bindings seen by sessions using branch B. Isolation must hold
while A's DDL is running, after commit, and after rollback. A change on A must not
require B to switch views, stop using its schema, or have its user reprepare a
statement because A's schema changed. Scope schema-conflict locks by branch/object as appropriate; a
transaction-duration database-wide DDL freeze is not the target architecture.
Ordinary shared server resources can still contend; logical isolation does not
promise zero shared-resource contention.

Do not import Dolt's DDL transaction semantics or introduce a new requirement to
combine schema and data edits into a Dolt-style transaction. Follow PostgreSQL's
rules for each supported command, including commands prohibited in transaction
blocks. PostgreSQL ordinary table DDL is itself transactional and can share a SQL
transaction with DML; preserve that native behavior, including command visibility,
abort and savepoints where supported. This is not an instruction to add implicit
DDL commits or split a native PostgreSQL transaction. A SQL transaction commit
and a version-control commit remain separate operations.

Earlier requirements for coordinated publication mean internal consistency under
the applicable PostgreSQL command/transaction boundary: no observer may see a
schema with incompatible data/index roots or a half-published dependency change.
They do not require adopting Dolt's transaction model. Existing standalone-command
rules for version-control functions are a separate concern.

### Ordered implementation work

1. **Canonical schema provider.** Decode canonical namespaces, tables, columns,
   row types, indexes, constraints and dependencies into native catalog views.
   Extend canonical formats where necessary, using portable object references.
   Hide physical definitions for versioned objects; missing required metadata or
   unsupported paths must reject instead of falling back. Keep PostgreSQL-owned
   bootstrap/system objects on their existing implementation.
2. **Automatic per-reference binding.** Ordinary names select the session branch
   and applicable SQL snapshot; qualified names and AS OF select their own
   contexts. Carry bindings through planning, execution, prepared statements and
   cursors. Syscache, relcache, type and dependency lookups must honor context or
   use handles explicitly bound to it. SQL pg_catalog/information_schema access
   must see the same definitions, including supported revision-qualified catalog
   references. No user preparation or persisted historical projections.
3. **Storage integration.** Resolve bound definitions to stable storage resources
   for locks, buffers and WAL, independently selecting the branch/historical data
   root. Runtime OIDs cannot stand in for cross-session lock/resource identity.
   Validate write authority and schema layout at every supported mutation path.
4. **Native DDL through provider state.** Construct provisional definitions in
   transactional provider state and publish supported canonical schema, dependency,
   storage and membership changes according to PostgreSQL's native rules above.
   Remove reliance on physical catalog name uniqueness for versioned objects.
   Exercise a narrow CREATE/index/rollback path early while building steps 1–3;
   do not defer discovering DDL integration problems until all reads are polished.
   Then pass independent same-name CREATE and extend to bounded ALTER/DROP forms.
5. **Lifecycle and compatibility closure.** Complete plan/portal pins, snapshot
   selection, invalidation/rebinding, context reclamation and dependency traversal.
   Implement parallel context transport or reject unsupported parallel paths.
   Validate recovery, read-only and standby reads, and matching Doltgres suites
   alongside native regression/isolation tests. Required correctness mechanisms
   belong in each earlier step; this step is not permission to postpone them.

### Acceptance gates

The first integrated read gate is a real query reading two incompatible canonical
revisions of the same table, with correct row types and matching catalog
introspection. It must work cold and without a preparation call or persisted
schema projection. Synthetic provider fixtures alone do not meet this gate.

The writable gate adds independent same-name CREATE on two branches, primary and
unique indexes, DML on each, cross-revision reads, and branch-scoped dependency and
permission checks. Keep the preserved Doltgres same-name tests. Add two-session
checks showing B remains usable and sees its own schema during A's open DDL
transaction, after A commits, and after A aborts or rolls back a savepoint. Include
B's existing cursor/prepared plan, and native same-branch conflict handling. Test
schema/index failures and restart/crash boundaries for complete old/new states.

Doltgres tests are the oracle for version selection and branch isolation;
PostgreSQL tests/rules govern native DDL transaction behavior when the engines
differ. Record intentional transaction differences explicitly rather than weakening
branch-isolation expectations. Report selected suites, counts, failures, unsupported
paths and untested cases; passing ordinary reads does not establish completeness.

### Implementation status at this revision

The unmerged `zachmu/branch-aware-catalog` branch implements native immutable-view
lookup/scan infrastructure and synthetic-provider tests. Production canonical
schema decoding, automatic binding and DDL publication are not connected. Existing
selected compatibility tests pass, but the preserved same-name CREATE suite fails.
No completed writable architecture or fundamental impossibility has been proved.
The next work is production integration through the gates above, not another
private-namespace workaround or an isolated cache-only milestone.

### Context coverage and catalog tuple storage exploration (2026-10-23)

Prioritize propagating immutable revision context through stored catalog views,
lookup consumers and cached queries, with broader routine Doltgres compatibility
coverage. Existing canonical CREATE/DROP and limited ALTER establish the mutation
path; this gate expands read-side coverage rather than adding DDL syntax.

Explore retaining PostgreSQL catalog tuple representations as prolly payloads in
branch-specific schema storage. This could let native DDL mutate catalog tuples
through a contextual storage interface instead of reimplementing each operation
in a separate canonical schema format. It is not ruled out, but simply copying
raw heap tuples is insufficient: transaction headers, physical TOAST references,
local OIDs, dependencies and serialized expressions contain nonportable identity.
An experiment should separate tuple values from heap MVCC metadata, normalize or
translate referenced identities, and establish contextual catalog writes, indexes,
cache invalidation and dependency traversal. Retaining tuple structure does not
alone prove all DDL supported. Defer this experiment until context coverage gives
us better evidence about those boundaries.

### Provisional native catalog mutation workstream (2026-10-23)

This status supersedes the older implementation-status paragraph above. Canonical
providers, automatic binding, independent same-name CREATE, limited ALTER/DROP,
SQL/PLpgSQL routines, native dependency closure and catalog lifetime tests have
landed. CatalogTupleUpdate has an explicit proposal sink, exercised by statistics
and column options. That sink alone does not let native multi-pass DDL reopen its
own intermediate definition. The next objective is a general mutation session,
not another collection of per-command canonical rewrites.

#### Contract

* Begin from the selected branch's snapshot-visible canonical database root and
  hold branch-scoped publication authority. Preserve PostgreSQL transaction,
  command-counter and savepoint behavior; other branches remain independently
  readable and writable.
* Create a private provisional catalog context. Existing immutable views, prepared
  plans, cursors and explicit runtime type handles must not change underneath
  their users. References within the mutation session use that session's handles.
* Native catalog INSERT/UPDATE/DELETE and indexed/syscache scans see staged tuples
  according to command visibility. Native relation/type caches must invalidate or
  refresh consistently when those tuples change. Do not store historical physical
  catalog projections or require a user preparation call.
* Preserve native tuple values through catalog-specific portable codecs. Identity
  references, dependency rows and expression trees require explicit translation;
  raw heap headers, TIDs, local OIDs and physical TOAST pointers are not payloads.
* Validate and publish the final schema, dependency and matching data/index state
  atomically. Rollback/cancellation releases all provisional state and allocations.
  Reject unsupported effects before they can reach physical catalog storage.

#### Implementation and acceptance order

1. Implement provisional tuple staging and command-visible catalog/cache refresh
   through a common interface, including insertion/deletion, scan/index behavior,
   resource cleanup and isolation from pinned immutable views.
2. Route native ALTER orchestration through that interface. Start with operations
   that force reopen/relookup, then cover mixed actions, constraints/defaults and
   tuple-layout changes. Expand the value/expression codecs with each affected
   catalog family; reuse native ordering, validation and dependency machinery.
3. Exercise normal CREATE/ALTER/DROP consumers against the same mutation contract.
   Unsupported object families or storage effects must be explicit and documented;
   passing one metadata-only command is not evidence of a general solution.
4. Validate statement/transaction atomicity, savepoints, failure at intermediate
   passes, prepared/cursor history, same-name objects and simultaneous independent
   branch DDL/DML. Compare supported native semantics with PostgreSQL regression
   tests and version/branch semantics with original Doltgres cases. Broaden the
   routine compatibility gate and record precise exclusions.
5. Run restart/crash/standby and cache-pressure tests. Write an HTML report listing
   architectural decisions, implemented coverage, failures and genuine blockers.

#### Remaining catalog completion work

Additional object families include stored views, independent user-defined types,
operators and sequences. Defaults, generated expressions, expression indexes and
parsed routine bodies require portable identifier-aware codecs. Native dependency
closure must receive their edges and publication effects; shared-role lifetime and
cross-database dependencies remain incomplete. Continue catalog-consumer audits
and name-resolution/introspection testing. Worker context transport, session-lived
handle/type-cache sizing and persistent history/storage garbage collection remain
operational work. Retain the unexplained nested-DDL timeout as a stress-test target.

#### Implemented provisional mutation checkpoint (2026-10-23)

The common mutation context now stages native catalog INSERT/UPDATE/DELETE,
validates identity constraints, exposes command-visible rows and refreshes local
caches. Savepoint rollback removes child mutations; pinned scans retain their
versions until release. Native multi-pass ALTER runs against this context with
original-caller expression evaluation. Portable codecs retain column/default/CHECK
values and reconstruct native tuples/dependencies on read.

The native rewrite executor can now scan the original root and write a private
prolly replacement stream through an optional table-AM callback. Final publication
compacts dropped attribute slots and atomically installs matching schema, rows and
rebuilt indexes. Tested coverage includes mixed ADD/DROP, constant/volatile defaults,
NOT NULL, CHECK validation, non-indexed ALTER TYPE USING, caller input settings,
statement cancellation, savepoints, simultaneous branch rewrites and crash recovery.

Next: declare and implement independent index lifecycle effects in the publisher;
indexed-column DROP/TYPE currently rejects before physical storage deletion.
Then add foreign-key, sequence/identity/generated and further object codecs, and
bring remaining CREATE/DROP consumers under the same contract. Stored expressions
currently accept audited built-in scalar node families; user-defined functions,
nonportable values and unsupported nodes reject. Nested utility commands reject
inside a provisional mutation until their publication effects are modeled.
Statistics fields are supported; branch-specific ANALYZE remains separate.
Global role lifetime across retained history, worker transport, session handle
reclamation and persistent GC remain open. Existing recovery/standby/cache-pressure
suites continue to run; sustained DDL stress remains a follow-on, not a claim of
production readiness.

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
