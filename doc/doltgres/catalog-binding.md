# Native versioned catalog provider

Versioned table queries bind automatically on first access. The persistent
materializer, `dolt_catalog_materialize`, and `pg_dolt_projection` were removed.
Neither an admin call nor a warmed cache is required. SELECT creates no persistent
catalog entries and does not reconstruct historical tables or indexes.

## Binding and execution

`dg_catalog.inc` resolves a revision through the manifest under the SQL snapshot,
then resolves the logical namespace/table in its retained database root. It builds
an in-memory TupleDesc from retained schema metadata. `RTE_DOLT` carries the table
object address and explicit column types, typmods and collations. Historical table
bindings have no physical relation OID; incompatible schemas can coexist in one
query. Ordinary tables and PostgreSQL access methods keep their existing paths.

`dg_catalog_scan.c` supplies a core CustomScan, `DoltVersionedScan`. Each historical
scan traverses retained manifest chunks lazily, with one node per tree depth in
memory. Leaves contain occurrence counts and packed tuples. Decoding uses the
bound TupleDesc, and duplicates are emitted separately. LIMIT can stop after a
single tree path. The executor uses the original RTE number and standard whole-row
copy rowmarks for UPDATE FROM / EvalPlanQual. It never opens a fabricated relation.

A qualified branch reference, e.g. `"postgres/main".public.t`, selects that branch's
snapshot-visible working root, independently of the session branch. When no local
root exists it scans retained HEAD chunks directly. `AS OF 'main'` selects the
committed revision. A live working binding requires a compatible live prolly
relation; unsupported/unpublished schema changes error rather than silently
substituting HEAD. Existing working-root loading still reads relation chunks
eagerly; lazy retained scanning does not fix that separate performance limitation.

Ref dependencies and conservative plan reanalysis preserve snapshot-sensitive
binding across executions, including after a REPEATABLE READ transaction ends.
Explicit branch references survive session checkout. Discarding plans changes no
availability guarantees. No descriptor cache is required for correctness.

## Permissions and safety

The caller needs retained namespace USAGE and table SELECT permission. Current
live objects additionally enforce current permissions, preventing historical
grants from bypassing a live revoke. Dropped objects use retained ACLs and current
role membership. This conservative authorization policy needs further review for
versioned roles/ownership and name reuse.

Fixed internal metadata SPI reads temporarily use an internal privileged identity,
restored on success/error; caller revision expressions are analyzed before that
switch. The internal search path explicitly puts pg_temp last to prevent temporary
objects from shadowing private catalogs. Users need no elevated privileges.

Stored views containing versioned bindings are explicitly rejected until view
owner authorization, dependencies and rebinding are implemented. Historical DML
targets and row locks are unsupported; historical reads as DML sources are tested.

## Verification

Native `test_prolly/011_catalog_binding` has 41 assertions covering cold read-only
access without an assigned XID, ordinary users, revocation, temp-name spoofing,
unchanged catalog counts, incompatible schemas, dropped tables, immediate restart,
physical standby, lazy LIMIT, whole rows/joins, UPDATE rechecks, prepared ref
movement, explicit branch binding and concurrent working-root visibility.

The shared external-server Doltgres harness passes TestAsOf, TestCatalogHistory,
TestNativeVersionedQueries, TestNativeHistoricalCursors and TestRevisionWorkingSets
on both engines (44 assertion statements, including cursor/transaction actions).
The four SELECTs in upstream queries.VersionedQueries are reused verbatim with
unchanged expected rows. Cursor coverage is an extracted subset, not a claim that
the entire original cursor suite passes. Merge and version-control guard suites
also pass against the fork. PostgreSQL core regressions and all native prolly/
fixture suites pass. See the workspace HTML report for exact coverage and logs.

## Remaining architectural work

This validates automatic table-read binding, not a complete versioned catalog.
Shared-schema DDL freeze remains. Branch-local DDL, schema merge, versioned custom
types/functions/sequences/views, catalog introspection, symbolic dependencies,
writable binding authority and retention/GC pins need explicit core integration.
Row-level publication and removal of table-wide writer serialization remain open.

Historical scans currently have no retained index paths/statistics or parallel
execution. Named historical composite types and system columns are unsupported.
Grammar accepts revision strings and constant-foldable GREATEST/LEAST expressions;
arbitrary expressions, parameters and timestamp lookup are not implemented.
Qualified identifiers obey PostgreSQL's 63-byte limit; use AS OF for full hashes.
Only the current database can be revision-qualified.

Continue with a native provider boundary, not persistent physical projection.
Table reads demonstrate that canonical descriptors can bypass physical OIDs;
they do not establish that general types/functions can avoid all OID infrastructure.

## Next architecture gate — 2026-10-22

The repository-root [plans](../../PLAN.md) are now version controlled. The
[branch-local DDL plan](../../CATALOG-DDL-PLAN.md) specifies the next viability
experiment: transactional schema isolation, dependency-consistent bindings,
versioned types/functions, concurrent readers and recovery. This extends the
native provider direction; it does not restore persistent materialization or
claim that the existing table-read milestone already solves generalized catalogs.
