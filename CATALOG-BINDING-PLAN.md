# Native versioned catalog provider

## Hard requirements

A valid versioned query must work on first access without user preparation,
privilege elevation, or warmed descriptors. This includes cold read-only
transactions, restart, and physical standbys. Cache presence may affect performance
only. There must be no persistent catalog writes or full-table reconstruction
caused by SELECT. Versioned query semantics follow the Dolt/Doltgres oracle;
tests must not add preparation calls.

The explicit materializer and its projection catalog have been removed, not kept
as an administrative prerequisite. The earlier retained-projection report describes
an obsolete experiment, not the selected implementation direction.

## Current experiment

Introduce a native versioned range-table binding with its canonical descriptor
address, typed in-memory columns, selected revision/root and write authority.
Execute retained table reads with a native lazy prolly scan, without inventing
physical relation OIDs or copying historical rows into relations. Determine through
cold-access tests which catalog paths can use these descriptors and which require
deeper provider integration. Do not assume unchanged cache keys or synthetic OIDs
are sufficient for general types, functions, dependencies and introspection.

## Acceptance and iteration

1. Remove all preparation calls from shared Doltgres history tests.
2. Pass cold first access, incompatible same-query schemas, dropped-table history,
   read-only/restart/standby queries, ordinary-user permissions, and lazy LIMIT scans.
3. Validate prepared-plan snapshot transitions, cursors and discarded caches.
4. Continue with revision-qualified branch working sets and the matching Doltgres
   tests, preserving branch semantics rather than substituting committed HEAD.
5. Expand types/functions/dependencies/introspection and writable schema generations
   as supported by experimental evidence. Keep architectural limits explicit.

Coordinate transactional catalog publication with [the manifest plan](MANIFEST-PLAN.md)
and [row-level publication](ROW-LEVEL-TRACKING-PLAN.md). Runtime descriptors are
reconstructible cache state, never canonical metadata or a correctness prerequisite.

## Milestone result (2026-10-21)

The native provider passes cold ordinary-user, read-only, restart and standby
reads without preparing catalogs. Incompatible retained schemas and two dirty
branch working sets coexist in a statement. Historical reads also work as INSERT
and UPDATE sources, including PostgreSQL EvalPlanQual rechecks. Prepared refs
respect SQL snapshots and rebind in the next transaction. The shared Doltgres
revision slice passes on both engines; see [the report](reports/versioned-catalog.html).

Proceed with the native provider boundary. Keep ordinary physical catalogs for
ordinary objects; do not restore persistent historical projection. The next gate
is transactional branch-local DDL and provider-aware dependencies/introspection,
then versioned types/functions/views/sequences. This is not proof that those paths
can keep unchanged cache keys. Retention pins and writable binding authority must
be designed alongside manifest publication and row-level tracking. Historical
index access/statistics and general AS OF expressions remain follow-up work.

## Direction and next gate — 2026-10-22

Continue with native versioned providers, not persistent catalog projection.
The read experiment supports this choice: independent descriptors and roots work
on first access, in read-only transactions and on standbys. Generalized catalog
viability remains unproven. In particular, table descriptors do not solve OID-based
type/function lookup, dependencies, stored expressions, authorization or catalog
introspection. These require deliberate core integration rather than scattered
name-lookup overrides.

The next gate is [CATALOG-DDL-PLAN.md](CATALOG-DDL-PLAN.md). Prove transactional
branch-local schema changes and reader stability, then incompatible user-defined
types/functions in a single cross-branch query. Every reference must carry enough
version context to resolve its dependencies consistently. Do not use a mutable
session-wide catalog switch to implement a multi-version statement. Runtime
mappings may be necessary; their exact representation is an experimental choice,
not a commitment to per-commit physical catalogs. The shared-schema freeze stays
in force outside the subset actually proven safe.

## DDL audit checkpoint — 2026-10-22

See [the audited contract](doc/doltgres/catalog-ddl-contract.md). A real-server
probe demonstrates different schema visibility between SQL catalog scans and
parser/relcache lookup under one old snapshot. Read descriptors cannot simply be
extended by mutating ordinary shared catalogs. Explicit write bindings, staged
schema membership and provider-aware dependencies are the next implementation
boundary. Type/function runtime identities and introspection remain unproven;
branch DDL is still guarded.

## Canonical schema provider and membership — 2026-10-22

The selected option 1 now has an implementation foothold. Retained table schemas
and namespaces are canonical prolly trees, decoded through one schema-object
provider. Working revision lookup uses the branch's working database descriptor,
with HEAD fallback only when no working target exists. Native parser/AM membership
checks prevent retained physical relations from leaking into ordinary reads,
writes and COPY on a branch where the table was dropped. Prepared ordinary
prolly queries depend on manifest refs and rebind after membership changes.

This is not a general replacement of OID-only catalog lookup. Raw catalog SQL,
regclass, type/function resolution and dependency infrastructure still use shared
physical identities. Preserve stable logical object identity; do not allocate a
permanent OID for every revision. The runtime identity/context bridge remains to
be proven with incompatible custom types and functions in one statement.
See [the implementation boundary](doc/doltgres/catalog-schema-provider.md).

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
