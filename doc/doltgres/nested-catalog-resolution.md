# Named types and nested SQL catalog context

Checkpoint: `zachmu/catalog-nested-resolution`, 2026-10-23.

## Contract and implementation

Named table composite types now resolve against canonical membership in the
selected database root. Qualified names, unqualified search_path lookup, regtype
input and type visibility use the same namespace ordering and permissions as
relation binding. Bootstrap and existing temporary types remain native candidates;
a canonical miss never falls back to a physical user type. An index with the
same name as a type in a later schema does not shadow that type.

SQL-language execution recovers a catalog context from object-ID arguments
(oid and supported reg* aliases) or concrete composite argument types. Actual
expression types resolve polymorphic arguments, including null composites;
internal fmgr callers without expressions retain a declared-signature fallback.
No arbitrary integer or composite pointer is interpreted as an object handle.
Unbound nested calls inherit the enclosing context. Multiple incompatible
contexts in one call reject explicitly; separate calls may use separate contexts.
PG_FINALLY restores the caller scope on success and error.

Table/catalog name binding, regclass lookup and named type lookup inside the
body use that inherited root rather than the session branch. Explicit revision
qualifiers still take precedence. Native relation visibility must also use the
enclosing root: an unversioned pg_catalog relation can be shadowed by a canonical
user relation in the selected revision. The existing compiled SQL-function cache
already keys entries by CatalogView; tests exercise one call site switching roots.
Scalar and set-returning inlining fences now recognize reg* aliases and composite
expression types, preserving the boundary that selects context.

A named composite or object-ID constant can reference a root without a table RTE.
Dependency extraction now records the manifest dependency for these expressions,
including before constant folding. Such prepared statements use the existing
conservative snapshot/ref reanalysis policy. This is correctness-first and not a
new fine-grained cache invalidation design.

Native operator resolution probes exact signatures with contextual type IDs
before considering generic record operators. The provider now includes bootstrap
pg_operator and pg_range rows: exact contextual misses are ordinary misses, and
native polymorphic resolution proceeds. This does not permit a fallback to a
physical user operator. Ordinary bootstrap function/operator overload selection
remains PostgreSQL's algorithm. User routines/operators are still rejected by
capture and unsupported DDL guards; no canonical pg_proc family is added here.

## Validation

The native TAP file `011_nested_resolution.pl` covers named casts and visibility,
index/type shadowing, missing branch types, nested regclass/regtype and polymorphic
calls, null composite arguments, SRFs in projections and FROM, incompatible scopes,
one-call-site cache switching, prepared statements across checkout and concurrent
schema changes, savepoint recovery after body errors, bootstrap record operators,
historical JSON conversion, and cold restart. Test SQL routines are shared helpers
created in pg_catalog before activation, not versioned user routines.

The Doltgres external-server gate now adds an activated-branch builtin-resolution
scenario, the original select-from-functions suite, 19 original operator scenarios
and 16 original SRF scenarios. The new eight-assertion shared scenario also passes
on Doltgres itself. Original expected values are unchanged. Exploratory failures
in eleven scenarios reproduce on the preceding fork build: error wording, numeric
infinity expectations, JSONB ordering, regex locale, and unsupported foreign-key
setup. See `testing/go/PROLLY-COMPATIBILITY.md` in the companion repository.

## Boundaries and next audit

This is not versioned CREATE FUNCTION, CREATE TYPE or CREATE OPERATOR support.
The canonical named types supported here are table rowtypes; their generated
array types are not supplied yet. Top-level named type references use the session
branch, while names inside contextual SQL use the selected call scope. There is
no new AS OF syntax on type names. Whole rows from independently qualified table
references already carry their distinct contextual types.

The next consumer audit should cover procedure/routine signature lookup (including
OID-vector keys), user routine bodies and dependency closure, PL/pgSQL and dynamic
SQL caches, and explicitly typed extended-protocol parameters across schema
changes. The new prepared tests validate SQL expression reanalysis, not every
wire-protocol parameter/result-schema combination. Expanding supported DDL remains
a separate mutation gate. No new physical projection, persistent per-revision
identifier, WAL format, storage layout or concurrency policy is introduced.

## Final results

The assertion-enabled build passes all 33 selected native groups, including 40
new nested-resolution checks, 231 unchanged core regression tests and 122
isolation tests. The expanded external Doltgres gate passes 27 suites, 105
scenarios and 919 leaf assertions. The new eight-assertion cross-engine scenario
also passes against Doltgres. Full exploratory/baseline logs and the HTML report
are retained in the workspace reports directory. No sqllogictest run or push was
performed.
