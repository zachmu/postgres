# Branch-local DDL viability gate

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

## Implementation sequence

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
