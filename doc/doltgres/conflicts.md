# Conflict tables, constraint violations and merge previews

Reference: Dolt `go/libraries/doltcore/sqle/dtables/{conflicts_tables_prolly,constraint_violations_prolly,schema_conflicts_table}.go`,
`merge/violations_unique_prolly.go`, `dtablefunctions/dolt_preview_merge_conflicts*.go`,
`dprocedures/dolt_verify_constraints.go`; Doltgres `testing/go/dolt_tables_test.go`,
`dolt_functions_test.go`.

## Merge artifacts

`dolt_merge` records two kinds of artifacts in the pending merge object:
`conflicts` (row-level, as before) and `violations`. A merged table whose rows
would violate a unique constraint no longer produces a whole-table conflict:
the colliding rows are recorded as Dolt "unique index" violations
(`{"Columns": [...], "Name": index}`) and the destination table keeps its own
rows, because the storage engine cannot hold duplicate unique keys. The merge
returns `conflicts = 1` ("conflicts found"), `dolt_status` reports
`constraint violation`, `dolt_merge_status.unmerged_tables` lists the table,
and `dolt_commit` refuses until the violations are cleared.

## Tables and functions

| Name | Notes |
|---|---|
| `dolt_conflicts_<t>` | `from_root_ish`, `base_*`, `our_*`, `our_diff_type`, `their_*`, `their_diff_type`, `dolt_conflict_id`, plus `base_/our_/their_cardinality` for keyless tables. Decoded from the pending merge's packed evidence with the table's working descriptor. |
| `DELETE FROM dolt_conflicts_<t>` | Rewritten in the parser (`dg_system_delete`) into `dolt_conflicts_clear(table)`, which rewrites the merge object without that table's entries; working rows stay as installed (ours). A WHERE clause is rejected; the command tag is that of a SELECT. |
| `dolt.constraint_violations`, `dolt_constraint_violations_<t>` | Summary (unqualified table, numeric count) and typed rows: `from_root_ish`, `violation_type`, table columns, `violation_info` (json, Dolt's compact `{"Columns":[..],"Name":..}` text). `DELETE FROM dolt_constraint_violations_<t>` clears them. |
| `dolt.schema_conflicts` | Doltgres columns; always empty because schema merges are rejected before installation. |
| `dolt_preview_merge_conflicts_summary(a, b)` | `(table, num_data_conflicts, num_schema_conflicts)` over the merge base of two branches; a table whose definition changed differently on both sides counts as one schema conflict with NULL data conflicts. Doltgres argument errors are reproduced (arity, NULL, empty and unknown branches). |
| `dolt_preview_merge_conflicts(a, b, table)` | Conflict rows computed by a three-way walk over revision tables (same rules as `dolt_merge`: column-wise merging with recursive JSON object merging, keyless cardinality conflicts) without installing anything; errors "schema conflicts found: 1" when the schema conflicts. Columns are derived at parse time like `dolt_diff`. |
| `dolt_verify_constraints([--all] [--output-only])` | Returns 0: unique and primary-key constraints are enforced by the storage engine and foreign keys/checks are not captured. |

## Limits

Only unique-index violations are produced (no foreign-key or check artifacts).
Schema conflicts are never produced. `dolt_conflict_rows()` remains as the
packed-evidence diagnostic. Revert and cherry-pick abort on violations.

Tests: `src/test/modules/test_prolly/t/022_conflicts.pl` (and the updated
`009_merge.pl`); Doltgres `TestPostgresDoltTables` conflict scenario and the
`DOLT_PREVIEW_MERGE_CONFLICTS` smoke tests. The original conflict and
constraint-violation scenarios run inside `START TRANSACTION` and are excluded
under the standalone-command contract.
