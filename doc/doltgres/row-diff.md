# Row-level diffs and generated per-table system tables

Reference: Dolt `go/libraries/doltcore/sqle/dtables/{diff_table,column_diff_table,blame_view,workspace_table}.go`,
`sqle/history_table.go`, `dtablefunctions/{dolt_diff,dolt_diff_stat}.go`; Doltgres
`testing/go/dolt_tables_test.go` and `dolt_functions_test.go`.

## Engine

`dg_diff.inc` loads a table at a revision as logical rows: keyed tables are
keyed by their packed primary-key values, keyless tables by canonical row
digest with an occurrence count. Historical tables come from the retained
`table` objects of a commit or staged state (schema JSON plus exported chunk
tree, imported into a private store); WORKING reads the live relation roots of
the session branch, through the canonical binding when branch views are active.
Tuple descriptors are rebuilt from the retained DGTABLE1 schema (pg_catalog
types, typmods and collations), so dropped or changed tables still decode.

`diff_rows` walks both sides by key and emits added, removed and modified rows.
Keyless cardinality changes emit one row per occurrence, as in Dolt. A primary
key that changed between the two revisions is rejected (Dolt: cannot diff keyed
and keyless schemas). Output columns are laid out as `to_<col>`, `to_commit`,
`to_commit_date`, `from_<col>`, `from_commit`, `from_commit_date`, `diff_type`;
an absent side borrows the other side's schema. Values are mapped by column
name, so added or dropped columns decode as NULL on the side lacking them.

## Parser integration

Functions whose output depends on their arguments cannot be declared with OUT
parameters. `dg_system_function_columns` (parse_clause.c, `transformRangeFunction`)
computes the column definition list for `dolt_diff(from, to, table)` and
`dolt_diff('a..b'|'a...b', table)` from constant arguments; non-constant
arguments fall back to PostgreSQL's usual "column definition list is required".

`dg_system_table` rewrites `dolt_diff_<t>`, `dolt_commit_diff_<t>`,
`dolt_history_<t>`, `dolt_blame_<t>` and `dolt_workspace_<t>` range variables
into function RTEs (`dolt_*_table('schema.t', ...)`) aliased by the written
name. The base table is resolved in the given schema or along the search path;
generated names take precedence over same-named user relations, matching
Doltgres. `transformSelectStmt` exposes the raw WHERE clause through
`ParseState.p_dolt_where`, so `to_commit = X` / `from_commit = X` conjuncts with
constant-foldable right sides (including `HASHOF('HEAD')`) become partition
hints; other predicates are evaluated by PostgreSQL over the full result.
`dolt_commit_diff_<t>` requires both hints, like Dolt.

Generated tables are not catalog objects: `pg_views`, `pg_class` and
`information_schema` do not list them, and `\d` cannot describe them.

## Functions and tables

| Name | Notes |
|---|---|
| `dolt_diff(from, to, table)` | Two-point diff; `to_commit`/`from_commit` carry the revision text, dates come from resolved commits (NULL for WORKING/STAGED). Unqualified table names resolve across schemas, preferring the caller's search path; ambiguity is an error. |
| `dolt_diff('a..b', table)` / `'a...b'` | Two-dot and three-dot (merge-base) ranges. |
| `dolt_diff_<t>` | WORKING vs HEAD, then every reachable commit against each parent, newest first. |
| `dolt_commit_diff_<t>` | Exactly one `from_commit`/`to_commit` pair; "invalid ancestor spec" for impossible `^N`/`~N`. |
| `dolt_history_<t>` | Rows at every reachable commit plus `commit_hash`, `committer`, `commit_date`. |
| `dolt_blame_<t>` | Latest non-removing change per primary key with commit metadata, ordered by key. Requires a primary key. |
| `dolt_workspace_<t>` | `id`, `staged`, `diff_type`, `to_*`, `from_*`; staged (HEAD→STAGED) then unstaged (STAGED→WORKING). Unknown tables yield an empty result. |
| `dolt.column_diff` | Per STAGED/WORKING/commit partition, columns added, dropped or with modified values. |
| `dolt_diff_stat(from, to[, table])` | Dolt's row and cell accounting; unchanged tables produce no row. |

`--author` now also sets the committer unless `--committer 'Name <email>'` is
given, following Dolt.

## Limits

Every partition re-imports the table at its commits; history-wide tables are
linear in commits × rows and intended for the current test scale. Schema
changes between revisions decode by column name only; type changes are not
coerced. Constraint-violation and conflict tables are a separate slice.

Tests: `src/test/modules/test_prolly/t/020_row_diff.pl`; Doltgres
`TestUserSpaceDoltTables` diff/history/blame/workspace/column_diff scenarios and
`TestPostgresDoltDiffFunctions` (the mixed schema-feature originals are excluded
by the prolly profile).
