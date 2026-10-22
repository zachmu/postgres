# Branch-local CREATE checkpoint

**Later checkpoint:** [same-name CREATE is now implemented](catalog-same-name.md).
The historical restrictions and test counts below describe this earlier milestone.

2026-10-22. Builds on [canonical schemas and DROP](catalog-schema-provider.md).
This is a bounded continuation of option 1, not the full DDL viability gate.

## Supported operation

After branch activation, native `CREATE TABLE` accepts ordinary permanent prolly
tables with supported built-in types, NOT NULL and immediate PRIMARY KEY / UNIQUE
constraints, including composite primary keys. An omitted AM selects
`dolt_prolly_table`; an explicitly different AM is rejected. Ordinary inactive
PostgreSQL CREATE continues through its existing path.

The selected branch alone gains canonical membership. Another branch's query
cannot access the retained physical table. Working and historical qualified reads
use the existing automatic provider. Existing member IF NOT EXISTS leaves its data
unchanged. Physical names already reserved by another branch/history cannot yet be
reused; IF NOT EXISTS does not silently turn that case into success.

## Publication and authority

1. The CREATE entrypoint checks activation and takes the existing workspace lock.
   Mutation requires Read Committed. Enabled event triggers are rejected for this
   slice. Creation namespace resolution and native CREATE execute as the caller.
2. A scoped runtime marker identifies only the newly allocated physical table.
   Native index subcommands are admitted only in utility subcommand context, for
   that table, with constraint indexes and without concurrent creation. It is not
   a global DDL-guard bypass. The scope is cleared even when native CREATE fails.
3. The new table and its indexes may initialize empty content without a matching
   HEAD member. This matters on non-main branches, whose first data access would
   otherwise try to import a nonexistent retained table.
4. Fixed-query privileged capture retains canonical schema/content and includes
   only existing branch members plus the newly created table/indexes. PostgreSQL
   still supplies their definitions through the physical catalog bridge.
5. Ref CAS publishes the successor working database descriptor. Physical objects,
   root records, chunks and ref update share native SQL transaction outcome and
   WAL recovery. Staged and HEAD are unchanged until add/commit.

The temporary relation marker contains runtime OIDs; those OIDs are not serialized
into canonical schema trees. It authorizes only creation-time capture/index work,
not general writes against arbitrary schema generations. No query-time materializer
or separate filesystem manifest is introduced.

## Findings and tests

- `015_branch_create.pl`: 36 native assertions pass. Coverage includes immediate
  DML, branch isolation, ownership/namespace permissions, transaction/savepoint
  rollback, IF NOT EXISTS preservation, composite/unique indexes, failed index
  cleanup, scope restoration, creation-namespace search-path rules, non-main empty
  roots, historical capture, restart and uncommitted crash rollback.
- Native regression: all 19 selected test groups pass (test_prolly, test_doltgres
  and the 231-test PostgreSQL regression suite), with assertions enabled.
- Doltgres harness: 131 assertions across ten focused suites pass on the fork;
  78 assertions across seven shared suites pass on Doltgres. The new
  TestBranchCatalogCreate is a 17-assertion shared scenario, run unchanged on both
  engines. The previous nine fork suites and six reference suites are retained.
  These are selected tests, not full Doltgres-suite conformance.
- The previous audit test loses its obsolete CREATE rejection and physical-absence
  checks (25 becomes 22 assertions); positive CREATE coverage replaces them.

Native index failure testing confirms that an error after physical table creation
rolls back the table and clears the creation scope. Namespace tests ensure CREATE
resolves its actual creation namespace, rather than incorrectly treating a table
in a later search-path namespace as the target.

## Restrictions and next implementation

CREATE LIKE/inheritance/partitioning/typed tables, temporary or unlogged tables,
custom tablespaces/options, defaults, generated columns, foreign keys/checks,
deferred constraints and enabled event triggers are not supported by this branch
CREATE slice. Other existing portable-type/capture restrictions remain. Standalone
CREATE INDEX, layout-changing ALTER, schema merge and branch-local namespace DDL
remain guarded. Broad workspace locking and session-selected writes remain.

The physical name/OID bridge is now the immediate blocker to broader DDL. A table
name cannot be independently reused by another branch, and changing shared
pg_attribute would still corrupt other branches' runtime definitions. The next
milestone needs an explicit logical identity + schema generation → runtime binding
mapping, with new generations allocated during DDL, not historical query-time
materialization. Test same-name CREATE on two branches, then ADD/DROP COLUMN with
row transformation, old snapshots/cursors, prepared plans, abort and crash recovery.

This mapping must preserve per-reference context for queries spanning branches.
The public logical catalog view, custom type/function dependencies and generic
OID-keyed caches remain unresolved; adding CREATE does not validate them. Raw
pg_catalog/information_schema/regclass still expose the retained physical bridge.
