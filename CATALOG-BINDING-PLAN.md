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
