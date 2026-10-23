# Catalog milestone 2: canonical routines

SQL and PL/pgSQL function definitions are database-root members. A `routine`
manifest object references one `schema` object containing a DGROUTINE1 property
map. The map retains pg_proc field values through PostgreSQL's type I/O, except
that namespace, role, language and signature identifiers are symbolic. Composite
arrays name their element type structurally rather than persisting a generated
array name. Runtime function/type/namespace OIDs are reconstructed in each
CatalogView; no physical per-revision pg_proc rows are created.

Native CreateFunction and ProcedureCreate still check signatures, replacement
compatibility, permissions and function bodies. Their final write goes to the
canonical working set. Validator errors roll the root publication back with the
SQL transaction. Initial capture converts existing supported routines as well.
Canonical DROP removes the current member, retaining immutable history. Retained
definitions are validated before publishing table deletion, so it cannot publish
a dangling function signature. General dependency-directed CASCADE is the next
milestone, rather than a separate ad hoc deletion graph here.

A resolved function handle determines the function definition. It must not be
looked up afresh in the body context inferred from arguments: shared helpers can
execute over historical catalog handles without having existed in that history.
CatalogViewFunctionTuple is the explicit runtime-handle interface used by fmgr,
result-type inspection and the compiled-function cache. Name resolution and SQL
pg_proc scans continue to use the selected canonical revision and do not expose
later physical user definitions.

Working-set calls carry an explicit FuncExpr flag. The flag survives node copying
and optimizer rebuilding. Such a call binds its body against the current working
branch, preserving read-your-writes and PostgreSQL statement visibility. An
immutable-context call retains its definition's revision context and lacks working
write authority. Canonical calls are conservatively excluded from constant folding
and SQL inlining until those transformations preserve the execution context.
Prepared-plan dependencies include function-only canonical expressions.

Native search_path caches retain physical namespace IDs. Consumers map those
entries into the selected CatalogView, instead of caching a backend-local revision
handle in session-global search_path state. Schema privileges come from canonical
namespace values. As with table CREATE, default privileges still come from the
shared default-ACL infrastructure; this is not versioned GRANT/ALTER DEFAULT
PRIVILEGES support. Event-trigger DDL is rejected rather than silently omitted.

## Boundaries

Supported canonical routines are SQL/PLpgSQL functions with string bodies,
built-in and canonical composite/array signatures, OUT/INOUT arguments,
configuration attributes, security attributes and captured ACLs. Parsed SQL bodies,
parameter defaults, transforms, planner support functions, procedures, aggregates,
custom implementation languages and custom non-composite signature types remain
explicitly unsupported. Native DDL before activation retains PostgreSQL behavior;
capture rejects definitions it cannot preserve. CREATE/REPLACE/DROP are supported,
not the general ALTER/GRANT/COMMENT routine command families.

This is not a schema-merge implementation. A genuine merge with differing routine
definitions still encounters the existing schema-merge restriction. Table-oriented
status/diff interfaces also do not yet present routines as Dolt's virtual schema
table. Explicit cross-revision routine-name syntax is not added in this gate;
checkout and historical pg_proc introspection exercise canonical definitions.

User views, independent user types and operators should follow the same contract:
canonical definition values, symbolic references, native catalog tuples in a
revision view, and a native DDL validation boundary. Stored expression trees need
an identifier-aware codec before their pg_node_tree values can be persisted. A
raw nodeToString or raw heap tuple containing OIDs is not a portable substitute.

## Validation

Native TAP covers capture, replacement, checkout, function-only prepared-plan
invalidation, transactional rollback, search_path cache changes, procedural
read-your-writes, another branch's isolation, overloads, composite signatures,
OUT parameters, ACLs, dependency rejection and historical catalog visibility.
The older historical-helper test now checks successful pg_proc introspection with
absence of a later physical helper, replacing its old unsupported-catalog check.
Core PostgreSQL regression expected outputs are unchanged.

Doltgres's original PL/pgSQL Branching scenario runs with standalone commit calls
and a separate log hash-length assertion accepting both supported hash widths.
This preserves its branch/function assertions and commit-result validation.
