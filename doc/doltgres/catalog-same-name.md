# Same-name writable catalog generations

2026-10-22. Completes the same-name CREATE step following
[catalog-runtime-map.md](catalog-runtime-map.md). This supersedes that checkpoint's
physical-name restriction and ambiguous-new-branch reuse rule. Root plans were not
edited.

## User operation

After branch activation, ordinary `CREATE TABLE public.t (...)` can independently
create different definitions on different branches. Each session resolves its own
working schema. A query can join `"postgres/main".public.t` and
`"postgres/other".public.t`, or their committed `AS OF` versions, without preparing
the catalog. INSERT/UPDATE/DELETE/COPY, constraint indexes, DROP/recreate, savepoints,
crash recovery and cold-standby reads use the selected resources. IF NOT EXISTS
checks selected-branch membership. Prepared queries/writes rebind on checkout;
PostgreSQL's restrictions on changing a prepared statement's result type remain.

## Three local catalogs, one canonical authority

- `pg_dolt_binding`: MVCC mapping from (branch, logical qualified name) and expected
  canonical schema address to runtime relation OID. Canonical membership still
  decides existence; stale bindings cannot resurrect dropped objects.
- `pg_dolt_namespace`: immutable runtime namespace OID → logical namespace OID.
  CREATE allocates a private `pg_dolt_runtime_<token>` namespace for each new table
  generation. Its table, indexes and composite types keep their unqualified names.
  Thus native name/OID-only caches see separate, internally consistent objects.
- `pg_dolt_generation`: one available-resource record per runtime relation, with
  logical name and captured schema address. Unlike current branch bindings, this
  survives their replacement. Branching from an older commit can reuse a retained
  compatible resource after the last current branch has replaced that schema.

These heap/WAL catalogs and physical relation allocation commit or abort together
with canonical object/ref publication. PUBLIC access to local metadata is revoked.
Internal names, runtime OIDs and transaction IDs never enter canonical schemas or
content hashes. The catalog version changed; tests initialize fresh clusters.
No per-commit physical schema copies or query-time catalog writes are introduced.

## Resolution, capture and permissions

`RangeVarGetRelidExtended` asks the provider for the selected logical binding before
performing its ordinary locking/retry/callback work. Search-path order and logical
namespace USAGE checks remain. System/temporary/ordinary AM lookup keeps its native
path. AM checks still enforce exact schema/resource identity. Explicit private
namespace references are rejected, including for table owners.

Creation checks logical namespace CREATE rights as the caller. Default privileges
come from that namespace. Table ACLs apply to the resolved runtime object. Retained
reads check canonical grants and, where available, the matching runtime resource's
current ACL; a different same-name table is not the ACL authority for that schema.
Raw catalog introspection is still physical, not a complete logical catalog view.

Capture enumerates exact canonical members and their bindings, plus the new table
and its indexes during CREATE. Native deparsing maps runtime namespaces to logical
names structurally. Automatic index naming checks the logical schema too; explicit
duplicate index names abort CREATE. Independent allocations of the same definition
produce identical canonical schema hashes.

## Compatible resources are not logical identity

A **new** branch name has no local roots, so any schema-compatible available table
resource can host its data: initialization imports that branch's own retained HEAD,
never the resource's other branch roots. Index resources must belong to the selected
table. Existing branch bindings remain exact; ambiguous repair fails closed.
Branch names cannot yet be deleted/reused. That restriction matters to this rule.

This is resource reuse, not an assertion that independently created objects have
common lineage. Stable portable object identity, rename and schema merge semantics
remain future work. A schema hash is a compatibility key, not identity. Historical
reads remain possible without a local resource; writable checkout still requires a
compatible retained resource rather than synthesizing one from arbitrary schemas.

## Findings and remaining limits

Testing found and fixed a DROP bug: the API's copied command snapshot could not see
a freshly captured database object when filtering its child list, losing unrelated
members. DROP now advances that snapshot before reading the capture. Parent mapping
selection during new-branch creation likewise sees mappings inserted earlier in
the command. Merge installation filters bindings by actual canonical membership.

This milestone retains the plain permanent-table/built-in-type CREATE subset:
NOT NULL and immediate PRIMARY KEY/UNIQUE constraints, including composite keys.
Defaults, generated columns, foreign keys/checks, partitioning/inheritance, standalone
CREATE INDEX, ALTER, namespace DDL and schema merge remain guarded. Writes select
the session branch. Broad workspace and mapping-writer locks remain; this is not
row-level concurrency. Runtime resources and stale mappings are retained without GC.
General named-composite/type lookup, functions, logical catalog introspection,
pg_dump/restore of the versioned database and branch-specific dependency management
are not established by this milestone.

Native coverage is in `017_same_name.pl` and `018_same_name_transactions.pl`.
Doltgres shared coverage is `TestBranchCatalogSameName`, alongside the existing
CREATE/DROP, revision-query and merge compatibility suites. See the workspace HTML
report for final run counts and scope.

Final validation (assert-enabled build): all 23 selected native groups pass,
including all 231 unchanged core regression tests, all 122 unchanged isolation
tests, test_prolly and test_doltgres. The new native suites contain 35 and 23
assertions. Doltgres harness: 180 assertions across eleven focused suites pass on
the fork; 153 across eight shared suites pass on Doltgres. The new same-name suite
contributes 49 identical assertions on both engines. This is selected compatibility
coverage, not the full Doltgres suite; sqllogictest was not run.
