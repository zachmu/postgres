# Phase 1 catalog decision

## Selected direction

Proceed with canonical symbolic metadata plus distinct runtime PostgreSQL catalog
objects for branch definitions (the projection/hybrid candidate). Keep deep
branch-aware catalog access as a fallback if the later coverage audit finds
projection untenable. This is a selected implementation direction, not a claim
that full branch-aware catalogs have been implemented.

The experiment uses real schemas as physical containers and maps explicit
branch-qualified relation names through one opt-in lookup hook. The physical
schema naming rule is only a test fixture. Production must use an object mapping
service, versioned definitions, invalidation rules and a lifecycle/retention
policy; it must not expose this naming convention as the durable identity ABI.

## Evidence and comparison

`test_doltgres` runs on PostgreSQL REL_18_6 with assertions enabled. The regression
and TAP suites demonstrate:

* The same logical relation name on two branches has different columns, built-in
  types, relation OIDs and, in another fixture, user-defined enum OIDs. Both
  descriptions coexist in one normal planned/executed query.
* PostgreSQL resolves permissions and row locks against the projected objects.
  A locked main-branch row blocks another main-branch writer, not a dev writer.
* A prepared plan in another backend replans after DDL on the projected relation.
  A result type change raises PostgreSQL's normal cached-plan error; a change
  on the other branch leaves the main branch's result descriptor intact.
* Transaction and subtransaction rollback undo DDL, runtime relation OIDs remain
  stable, and REPEATABLE READ retains a consistent old view of both relations.
* Prepared changes to both projected relations survive an immediate shutdown,
  remain invisible after restart, and become visible together on COMMIT PREPARED.
  This exercises ordinary heap WAL; it is not a test of prolly WAL recovery.

| Candidate | Interfaces requiring attention | Phase 1 evidence |
| --- | --- | --- |
| One session branch context over unchanged catalog identities | Cannot supply two incompatible descriptors for one cached relation/type identity | Insufficient design; same-query requirement rules out a session-only switch |
| Fully branch-aware catalog access | Catalog lookup, relcache/syscache/typcache identities, dependencies, lock keys, plan invalidation, bootstrap and DDL | Source/interface audit only; not implemented or benchmarked here |
| Distinct runtime objects with canonical metadata projection | Name binding, canonical-to-runtime mapping, metadata materialization and invalidation | Native same-query, prepared-plan, DDL, locking and recovery tests pass without changes to planner search, executor operators or existing cache keys |

The decision is based on demonstrated reuse of PostgreSQL's object machinery,
not a claim that the alternative cannot work. Giving each concurrently visible
branch definition its own runtime identity also addresses the core requirement
that a direct catalog implementation would eventually need to solve.

## Actual core patch

* `src/include/catalog/namespace.h`: declares `rangevar_projection_hook_type`.
* `src/backend/catalog/namespace.c`: optional projection at the start of
  `RangeVarGetRelidExtended`, before ordinary lookup/permission/lock processing.
* The test module returns a copy of the input RangeVar. Parse trees retained by
  prepared statements are never rewritten in place.
* With no module loaded, the hook is NULL and normal cross-database rejection
  remains intact. The module is not installed or loaded by a normal core install.

No cache, planner, executor, transaction, lock-tag or wire-protocol layout was
changed. Only relation lookup is projected. CREATE namespace selection,
branch-qualified type/function lookup, four-part column-name matching and
catalog introspection are not covered; query aliases avoid the latter column
qualification issue in the experiment.

## Required follow-up before phase 4

1. Canonical definitions must be serialized symbolically, including embedded
   expression/type references. OID allocation never becomes canonical input.
2. Define mappings for namespaces, relation row types, enum/domain types,
   functions, operators, constraints and dependencies; handle rename/drop.
3. Design branch/ref snapshots and runtime object retention. This experiment
   does not materialize a catalog per historical commit.
4. Define prepared-plan semantics across checkout: a plan binds a branch at
   analysis; changing a session default must not silently change that binding.
   Rebinding, where desired, requires explicit invalidation/reanalysis.
5. Provide transactional schema+data+index publication coordinated with WAL.
   Existing heap atomicity alone does not solve the future mixed-storage case.
6. Provide branch-aware catalog introspection and authorization policy. Physical
   fixture schemas currently remain visible through ordinary pg_catalog views.
7. Audit stored expressions, SQL-language function bodies, regclass/regtype
   values, extension catalogs and tooling that might leak runtime OIDs into
   versioned data or assume physical namespace names are canonical.

## Reproducing tests

Configure a separate build tree with `--enable-cassert --enable-debug
--enable-tap-tests`, then build PostgreSQL. The native regression and 18 TAP
assertions run with:

```
make -C src/test/modules/test_doltgres check
```

Run the full core regression suite with `make check`. Test nodes are temporary;
TAP starts its own servers. Use a filesystem that supports Unix socket chmod
(for example /tmp) for external servers and socket directories.
