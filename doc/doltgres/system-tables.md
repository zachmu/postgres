# Dolt system tables and the dolt namespace

Reference: Doltgres checkout 0a7ac15a, `server/tables/dtables` and
`testing/go/dolt_tables_test.go`; Dolt `go/libraries/doltcore/sqle/dtables`.

## Naming

Every database has a built-in `dolt` schema created by initdb. System tables
are views in that schema (`dolt.branches`) with `dolt_`-prefixed aliases in
`pg_catalog` (`dolt_branches`). A schema-qualified alias such as
`public.dolt_branches` or `newschema.dolt_status` resolves to the pg_catalog
alias when the schema exists. User relation and namespace names beginning
with `dolt` are reserved, including CREATE and rename paths. Internal versioned
metadata tables are the exception: explicit `public.dolt_docs`, `public.dolt_tests`
and `public.dolt_query_catalog` bind to their canonical branch definitions.
`SET search_path = dolt` makes the bare built-in names (`branches`) visible.
The `dolt` schema cannot be recreated and is not captured into commits; pg_dump
skips it like `pg_catalog`.

Resolution hooks: `dg_system_relation` (`dg_system.c`) in
`RangeVarGetRelidExtended`, and `system_alias` in the canonical catalog provider
so the aliases keep working once branch views are active. The `dolt` schema is
a native namespace for the provider, like `pg_catalog`.

`dolt_show_system_tables` is accepted as a session setting for Doltgres
compatibility; generated per-table system tables are not yet listed in
`pg_views`.

## Repository initialization

`dolt_init()` creates the repository; initdb runs it in template1, so every
database starts with branch `main` and an "Initialize data repository" commit
whose state records the initial namespaces (`public`). This replaces lazy
initialization on the first mutation. Doltgres additionally records a
CREATE DATABASE commit; that artifact is deliberately not reproduced.

Default committer/tagger email is `role@client-host` for TCP connections and
`role@localhost` for local connections, matching Doltgres's `user@127.0.0.1`.

## Implemented tables

| Table | Source | Notes |
|---|---|---|
| `dolt.branches` | `dolt_branches()` | name, hash, latest committer/date/message, remote, branch, dirty, latest author. `dirty` compares the session branch's live contents with its head; other branches use their persisted working set because DML does not publish working refs. remote/branch are empty until remotes exist. |
| `dolt.remote_branches` | SQL over `remotes/%` refs | Empty until fetch is implemented. |
| `dolt.commits` | `pg_dolt_commit_visible` | Commits reachable from any branch, tag or remote ref; newest first. |
| `dolt.commit_ancestors` | `pg_dolt_commit_visible` | One row per parent; root commits have a NULL parent and index 0. |
| `dolt.log`, `dolt.tags` | existing functions/refs | Moved into the dolt schema; pg_catalog aliases retained. |
| `dolt.status` | `dolt_status()` | HEAD vs STAGED (staged) and STAGED vs WORKING (unstaged) tables and schemas: new table, modified, deleted, new schema, deleted schema; tables with unresolved merge conflicts report `conflict`. Conflict rows first, then tables, then schemas, each sorted by name. |
| `dolt.diff` | `dolt_diff_changes()` | STAGED and WORKING table deltas, then each reachable commit against its first parent. Added/dropped tables are data changes only when they had rows. |
| `dolt.merge_status` | `dolt_merge_status()` | is_merging, source (revision text), source_commit, target (`refs/heads/<branch>`), unmerged_tables (unqualified, comma separated). The merge object now records `source`. |
| `dolt.conflicts` | SQL over the pending merge | Unqualified table name and numeric count, like Doltgres. |
| `dolt.remotes` | `pg_dolt_remote` | `dolt_remote('add', name, url)` / `('remove', name)`. Database-local configuration. |
| `dolt.docs` | `public.dolt_docs` (prolly) | Insert/update/delete through `dolt.docs` or `dolt_docs`. `AGENT.md` defaults to Dolt's embedded text until overridden. Documents are versioned with ordinary table data; the default text is virtual before allocation and seeded when the table is first created. |

`dolt_hashof(revision)` and `hashof(revision)` return the commit hash of a
revision expression.

## Limits

Docs, remotes and the dolt schema are outside versioned state. `dirty` for
non-session branches reflects persisted working sets only. Generated per-table
system tables, constraint violations, schema conflicts, ignore patterns,
schemas/procedures, rebase and statistics are separate slices.

Tests: `src/test/modules/test_prolly/t/019_system_tables.pl`; Doltgres
`TestUserSpaceDoltTables` (with profile exclusions for PostgreSQL error wording,
DESCRIBE and the bootstrap commit) and `TestPostgresDoltTables`.
