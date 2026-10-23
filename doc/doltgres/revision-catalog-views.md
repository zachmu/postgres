# Revision-qualified catalog views and relation name resolution

Checkpoint: 2026-10-23, `zachmu/revision-catalog-views`.

## Context contract

A bootstrap catalog view reference carries its selected immutable database root
in the RTE. AS OF selects a committed revision; database/branch qualification uses
the branch's snapshot-visible working root. View expansion copies this context
into the copied rule query, recursively through nested queries and view references.
Sibling references remain independent. Stored relcache rules are never modified.
Supported underlying catalog scans use the inherited root instead of selecting
the current session branch again. The bootstrap view's SQL definition remains the
server's definition; this is not versioned user-defined-view support.

Parser binding opens the bootstrap view through ordinary PostgreSQL machinery,
with revision syntax removed only after selecting its root. That preserves native
relation locking, aliases and permission entries. Query serialization now retains
context on view RTEs and native relation/permission fields on rewritten catalog
RTEs. Forced copy/serialization testing found the previously missing permission
index; it is now preserved. Prepared symbolic references rebind after HEAD changes;
already-open cursors retain their original context. No schema materialization,
physical catalog projection, or per-revision persistent relation ID is introduced.

## One ordered relation resolver

SQL table binding and the native RangeVar/regclass lookup hook now share ordered
symbolic resolution. Capture the caller's search_path before begin_api temporarily
sets its internal path. Honor explicit pg_catalog position, otherwise its implicit
priority; expand $user as the caller; skip nonexistent namespaces and implicitly
inaccessible namespaces; preserve quoted names, duplicates and empty paths.
Explicit inaccessible namespace lookup errors. Table privilege failure after a
name is selected does not fall through to another schema.

Bootstrap catalog names and views participate in the same ordered path: a user
relation can shadow them when pg_catalog is explicitly placed later. Existing
session temporary relations retain native lookup/opening and normal implicit or
explicit pg_temp priority; historical temporary references are rejected. Native
bootstrap/temporary matches retain PostgreSQL's normal locking path.

pg_table_is_visible previously compared synthetic namespace OIDs against the
physical path and returned false for canonical objects. It now resolves the name
inside the object's own provider context and compares the selected handle. This
also lets native deparsers make contextual qualification decisions.

## Verification

All 32 native groups pass (test_doltgres, test_prolly, core regression and isolation),
including 231 upstream regression tests and 122 isolation tests. No upstream SQL
or expected output changed. The final expanded search-path test independently
passes 42 checks; revision-view tests pass 15 checks. Coverage includes:

- Simultaneous incompatible revisions, branch qualifiers and AS OF, aliases,
  nested catalog views, index deparsing, prepared plans, cursor pins during HEAD
  changes, forced query-tree serialization, read-only access and cold restart.
- Schema ordering, implicit/explicit pg_catalog, catalog-name shadowing,
  information_schema through search_path, quoted schema/relation names and commas,
  missing/repeated/empty paths, caller $user, privileges, prepared path/role changes,
  SET LOCAL/rollback, branch-isolated UPDATE, INSERT/DELETE, COPY, LOCK and existing
  session-temporary relations.

The routine Doltgres gate now passes 23 suites, 68 scenarios and 339 leaf assertions.
It adds two focused suites and five original TestSchemas scenarios. The new
TestBranchSearchPath passes against Doltgres itself. Directly probing the four
revision-view assertions on Doltgres produced three failures: its qualified
catalog views returned the session branch. Preserve the required independent-view
semantics in TestPostgresRevisionCatalogViews, explicitly skipped on Doltgres until
that provider is fixed. This is a documented oracle limitation, not an emulated
behavior. Two additional original schema scenarios fail only their expected error
text; native tests cover the corresponding absence/error semantics. Their original
assertions are unchanged and they are not in the portable routine selection.

## Boundaries and follow-up audit

The search-path matrix is systematic coverage of supported relation and catalog-
view consumers, not proof that every PostgreSQL namespace consumer is integrated.
Named type/function/operator resolution, arbitrary stored SQL-function bodies,
user-defined view definitions, utilities beyond tested paths, and broader
namespace/type/routine/dependency versioning still need integration and audit.
Existing temporary relations can be resolved; this change does not provide
versioned temporary DDL or add temporary tuples to canonical catalog introspection.
Unsupported versioned object families remain outside this gate. Same-branch writer
serialization and canonical DDL scope are unchanged.

Keep the next audit focused on consumers that resolve by name or enter nested
execution without a relation-bound handle. Do not infer that retaining a root on
view references automatically supplies that context to arbitrary function bodies.
