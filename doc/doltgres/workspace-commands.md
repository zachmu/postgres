# Workspace commands, branch management and commit-graph helpers

Reference: Dolt `go/libraries/doltcore/sqle/dprocedures/{dolt_add,dolt_reset,dolt_clean,dolt_rm,dolt_checkout,dolt_branch,dolt_tag,dolt_revert,dolt_cherry_pick,dolt_count_commits}.go`,
`dtablefunctions/{dolt_branch_status_table_function,dolt_reflog}.go`,
`dfunctions/{merge_base,version,hashof_table,hashof_database}.go`; Doltgres
`testing/go/dolt_functions_test.go`.

## State composition

Staged and working states are immutable database objects. `compose_database`
(`dg_workspace.inc`) builds a state from a base state by replacing the members
of selected tables (and the indexes their schema lists) with the versions in a
source state, removing them when the source lacks them, and carrying over the
namespaces those tables need. Named `dolt_add`, `dolt_reset <table>`,
`dolt_rm --cached` and mixed resets are all expressed through it. Because a
capture publishes objects under a temporary snapshot, `capture_visible`
advances the API snapshot before composed states are read back.

## Commands

| Command | Behavior |
|---|---|
| `dolt_add(table, ...)` | Stages only the named tables (qualified or resolved along the search path). `.`/`-A` unchanged. |
| `dolt_reset()` / `('.')` | Unstages everything (staged ← HEAD). `dolt_reset(table, ...)` unstages tables. `dolt_reset(rev)` moves HEAD and staging (mixed). `--soft rev` moves HEAD only. `--hard [rev]` restores every table in the target state, drops working tables the target lacks, and moves HEAD, staging and working; it refuses targets whose tables are missing from the working set (recreating tables needs DDL). |
| `dolt_clean([--dry-run] [table ...])` | Drops untracked tables (present in neither HEAD nor staging) through DDL. |
| `dolt_rm([--cached] table ...)` | Removes tables from staging; without `--cached` also drops them. |
| `dolt_checkout(table, ...)` | Restores working tables from HEAD; tables absent from HEAD are dropped (Dolt's MoveTablesFromHeadToWorking). Returns `(0, '')`. Unknown names: "tablespec 'x' did not match any table(s) known to dolt". Tags or hashes: the Dolt detached-HEAD error. Checking out the session branch reports "Already on branch". |
| `dolt_branch` | `-d`/`-D [-f]` deletes (not the session branch; refs, pending merge and runtime bindings go), `-m [-f] old new` renames, `-c [-f] src dst` copies. Rename and copy require a clean source branch because branch-tagged relation roots cannot be re-labelled; the new branch starts from the source head. |
| `dolt_tag('-d', name)` | Deletes a tag. |
| `dolt_revert(rev...)` | Three-way application of each commit's inverse (base = commit, ours = HEAD, theirs = parent) onto a clean working set, committed as `Revert "<message>"`. Conflicts abort the command. Returns `(hash, 0, 0, 0)`. |
| `dolt_cherry_pick(rev)` | Applies one commit's changes (base = parent, ours = HEAD, theirs = commit) and commits with its message. |
| `dolt_count_commits('--from', a, '--to', b)` | `ahead` = commits reachable from `b` but not `a`; `behind` the reverse. |
| `dolt_branch_status(base, branch...)` | `(branch, commits_ahead, commits_behind)` with numeric counts, as in Doltgres. |
| `dolt_merge_base(a, b)` | Unique common ancestor; criss-cross histories are rejected like `dolt_merge`. |
| `dolt_version()` | `<PostgreSQL version>-dolt`. |
| `dolt_hashof_table(table)` | Content root of the working table. |
| `dolt_hashof_db([rev])` | Database state hash: WORKING (default, persisted on demand), STAGED, HEAD, branches, tags, hashes. |
| `dolt_reflog([ref] [--all])` | Head and tag moves recorded by the manifest CAS in `pg_dolt_reflog`; newest first; `ref` may be a branch, tag or `refs/...` name. Deletions are not listed. |

The standalone-statement contract now accepts a single version-control call
wrapped in scalar expressions (`substring(dolt_revert('HEAD')::text, ...)`),
still with constant arguments and no FROM clause.

The dirty-merge error is Dolt's "error: local changes would be stomped by merge".

## Limits

Revert and cherry-pick require identical schemas and a clean working set and
do not persist conflicts. Hard reset cannot recreate dropped tables. Branch
rename/copy of dirty branches is refused. Reflog does not record deletions.

Tests: `src/test/modules/test_prolly/t/021_workspace.pl`; Doltgres
`TestDoltBranchStatus`, `TestDoltTag`, `TestDoltProcedureRecordResults`, the
workspace smoke tests, `TestUserSpaceDoltTables/dolt_workspace_with_tablename`
and `TestPostgresWorkspaceCommands` (the mixed-schema originals are excluded).
