# Per-schema Dolt tables, patches and schema diffs

Reference: Dolt `go/libraries/doltcore/doltdb/{ignore,table_name_patterns}.go`,
`env/actions/staged.go`, `sqle/dtables/{ignore_table,schema_table,procedures_table}.go`,
`sqle/dtablefunctions/{dolt_patch,dolt_schema_diff}.go`, `dprocedures/{dolt_gc,dolt_undrop,
dolt_purge_dropped_databases,dolt_thread_dump}.go`; Doltgres `testing/go/dolt_tables_test.go`
("dolt ignore", "dolt schemas", "dolt procedures"), `dolt_functions_test.go` (patch, schema diff),
`create_view_test.go`.

## dolt_ignore

Doltgres keeps a `dolt_ignore(pattern, ignored)` table in every schema. Here it
is an ordinary versioned table (`dolt_prolly_table`) that `INSERT INTO
[schema.]dolt_ignore` creates on first use (`dg_system_insert_target` in the
parser). While no table exists, `dolt_ignore` reads as an empty generated
table, so `SELECT * FROM dolt_ignore` and `SELECT dolt_ignore.pattern FROM
public.dolt_ignore` work from the start. An explicit schema that does not
exist raises "table not found: schema.dolt_ignore"; an unqualified name uses
the first user schema on the search path (a real table found through the
search path wins).

Patterns follow Dolt: `*` and `%` match any sequence, `?` one character,
matched against the unqualified name of tables in that schema. A table is
ignored when a true pattern matches and no false pattern matches; more
specific patterns override less specific ones (Dolt's
`resolveConflictingPatterns`), and contradictory patterns leave the table
tracked. `dolt_rebase` is always ignored. `dolt_add -A`/`.` and `dolt_commit
-A` stage everything except ignored *untracked* tables (`staged_capture`), so
`dolt_status` still lists them as unstaged new tables, exactly like Doltgres;
adding an ignored table by name tracks it. Other commands (clean, reset,
checkout) do not consult the patterns.

## dolt_schemas and dolt_procedures

Generated per schema from the catalog (`dolt_schemas_table(schema)`,
`dolt_procedures_table(schema)`), resolved like `dolt_ignore`:
`dolt_schemas` lists the views of a schema as `(type, name, fragment, extra,
sql_mode)` with `fragment` rebuilt from `pg_get_viewdef` and `extra` =
`{"CreatedAt":0}`; `dolt_procedures` lists `prokind = 'p'` routines as
`(name, create_stmt, created_at, modified_at, sql_mode)`. Views and routines
are not captured by version control (the capture rejects them), so these
tables are never versioned and never appear in diffs; the Doltgres
assertions that expect `dolt_schemas` in `dolt_diff_summary` are excluded in
the prolly profile.

## dolt_patch and dolt_schema_diff

`dolt_patch(from, to [, table])` returns `(statement_order numeric,
from_commit_hash, to_commit_hash, table_name, diff_type, statement)`: per
changed table (search-path resolution, qualified `table_name`), schema
statements first (`CREATE TABLE` in Doltgres's layout, `DROP TABLE`, `ALTER
TABLE ... ADD/DROP COLUMN`), then data statements (`INSERT`, `UPDATE` of
changed columns, `DELETE` by primary key or full row). Values use Dolt's
quoting (`'` and JSON `"` escaped with backslashes; numbers and booleans
bare). Commit hashes are the fork's 64-character hashes; `WORKING`/`STAGED`
keep their labels. `dolt_schema_diff(from, to [, table])` returns
`(from_table_name, to_table_name, from_create_statement,
to_create_statement)` with empty strings for a missing side. Primary-key
changes produce no row-level statements.

## Administrative procedures

`dolt_gc([--shallow|--full])`, `dolt_purge_dropped_databases()` and
`dolt_thread_dump()` are no-ops (manifest objects are retained; PostgreSQL
drops databases immediately). `dolt_undrop(name)` always errors because
`DROP DATABASE` is not versioned.

## Fixes made alongside

`dolt_diff('a', 'b')` without `..` now errors instead of crashing in the
parser layout hook, and `dolt_diff_summary` resolves its table argument with
the shared search-path-aware resolver (an unqualified name that exists in
several schemas picks the first one on the search path).

Tests: `src/test/modules/test_prolly/t/023_schema_tables.pl`; Doltgres
`TestUserSpaceDoltTables/{dolt_ignore,dolt_schemas,dolt_procedures}`,
`TestCreateViewStatements`, and the `dolt_patch`/`dolt_schema_diff` smoke
scenarios under the postgres profile.
