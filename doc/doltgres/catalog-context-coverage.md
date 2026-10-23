# Catalog context coverage

Checkpoint: 2026-10-23, `zachmu/catalog-context-coverage`.

Stored catalog views bypass RangeVar parsing: their saved query trees contain
native relation RTEs. During recursive view/rule expansion, supported catalog
references now bind through the same immutable provider used by direct SQL.
Catalog column layouts are bootstrap-stable; existing Vars remain valid. The
result is an ordinary versioned scan carrying its canonical database root, not a
physical schema projection or a database-global switch. Native permission entries
are retained, including the view owner's checkAsUser identity. Direct parser
binding continues to check caller permissions. Unsupported catalog families are
not synthesized by this mechanism.

The provider's full catalog family list now governs direct SQL and stored-rule
binding alike, including dependency, attribute default, trigger and statistics
families. Bootstrap information_schema views enter normal PostgreSQL expansion;
their underlying supported catalogs use branch definitions. This makes ordinary
pg_tables, pg_indexes, information_schema.columns, table_constraints and
key_column_usage useful after activation. Native deparsers consume contextual
runtime handles returned by these scans. AS OF on supported base catalogs also
recognizes implicit pg_catalog qualification.

The work also fixes CTE precedence: unqualified CTE/ephemeral names must resolve
before canonical table membership. A same-named versioned table must not steal a
CTE reference. Explicit revision, database or schema qualification instead selects
a relation. Recursive CTEs and materialized CTEs exercise this boundary.

## Tests and repeatable compatibility gate

Native `008_catalog_context.pl` covers independent sessions, current membership,
prepared queries across checkout and schema changes, transactional DDL/abort,
repeatable-read snapshots and cursors during concurrent DDL, native catalog view
permissions, read-only expansion, CTE shadowing/recursion, mixed current/historical
catalogs, index deparsing and cold restart. The earlier canonical SQL test now
checks pg_tables returns the canonical-only table instead of expecting rejection.
No upstream PostgreSQL regression SQL or expected output is changed.

Doltgres `testing/scripts/catalog-context.sh` is the repeatable external-server
gate. Set DOLTGRES_TEST_SERVER_BINARY to the installed fork postgres executable.
It builds its own test binary, uses the existing fresh-state harness, and runs the
previous 12 version/catalog suites plus the new cross-engine introspection suite,
information-schema tables/type-length helpers, CTE/recursive suites, the existing
branch-checkout information-schema scenario, an index-column metadata scenario,
and XML/default introspection scenarios. Some added tests run before canonical
activation; they check native compatibility, not versioned DDL support. The new
introspection test explicitly activates branches and also runs on Doltgres.

The wider exploratory run is not all green. Original catalog tests contain fixed
Doltgres OIDs, exact system inventory/EXPLAIN expectations, btree deparser strings
and different constraint naming. Other cases require unsupported ALTER constraints,
triggers, persisted OID/XID values, or Doltgres-specific settings. The gate selects
portable scenarios explicitly; it does not rewrite expected values, silently skip
failing assertions, or claim full pgcatalog/information_schema conformance. Existing
prolly profile exclusions remain visible in the harness. Doltgres calls its default
primary-key constraint PRIMARY in information_schema, whereas PostgreSQL uses the
native generated name; the new shared test checks constraint type and key columns.

## Limits and next coverage

This is context propagation through stored bootstrap views, not support for
versioned user-defined views/functions or additional DDL operations. AS OF and
revision-qualified information_schema/pg_catalog *views* still need an inherited
context on view references; currently historical introspection uses supported base
catalogs explicitly. Cross-revision base catalog scans can coexist with current
stored views in one query. Catalog search-path shadowing, SQL function bodies,
namespace/type/routine/dependency closure and utility paths need further audit.
Unrepresented object families must not be interpreted as full DDL support.
Statistics synthesized for canonical objects remain empty; this is correctness
plumbing, not an optimizer statistics implementation. Same-branch writer
serialization, broader catalog mutation and row-level concurrency remain unchanged.

CATALOG-DDL-PLAN.md records the separate future experiment of storing native
catalog tuple values in prolly trees. Raw heap bytes are insufficient because
MVCC headers, local identifiers, external TOAST and serialized dependencies need
portable identity and contextual mutation rules.

## Verified results

The final assertion-enabled build passes all 30 native groups (test_prolly,
test_doltgres, core regression and isolation), including 25 new catalog-context
checks, 231 core regression tests and 122 isolation tests. The repeatable Doltgres
gate passes 20 suites, 61 scenarios and 302 leaf assertions; the new 16-assertion
branch introspection scenario also passes against Doltgres itself. An intermediate
compatibility run collided with a local binary reinstall (Text file busy); the
reported final run used an unchanged installation and completed successfully.

Follow-up: [revision-qualified views and ordered relation resolution](revision-catalog-views.md)
closes the historical bootstrap-view boundary above and expands search-path,
visibility and serialization coverage. It records the remaining object-family
and nested-execution audit boundaries.

Follow-up: [named types and nested SQL execution](nested-catalog-resolution.md)
connects canonical rowtype names, object-ID aliases and polymorphic arguments to
nested SQL binding, adds manifest dependencies for type-only expressions, and
records the bootstrap operator/range closure and remaining routine-family audit.
