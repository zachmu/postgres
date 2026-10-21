# First version-control command slice

Reference: Doltgres checkout 664df6bb, especially server/functions/dolt_procedures.go,
testing/go/dolt_procedures_record_test.go and testing/go/dolt_functions_test.go;
Dolt checkout 72e0a00a1a, sqle/dprocedures/{init,dolt_commit,dolt_branch,dolt_tag}.go,
sqle/dtablefunctions/dolt_diff_summary.go and sqle/dtables/log_table.go.

Use variadic text arguments and named OUT columns: commit -> hash text;
add/branch/tag -> status bigint. Log has the reference's twelve columns; diff
summary has from_table_name, to_table_name, diff_type, data_change, schema_change.
Hashes remain this engine's 64-character SHA-256 hex, not Dolt's 32-character hash.

This milestone captures the single live main branch and supports staging all
objects, commits, branch/tag creation, revision traversal, log and diff summary.
It does not introduce checkout, live branch projections, row-level publication
merging or full PostgreSQL schema coverage. Unsupported flags/features must error.

Commit and branch/tag mutation calls run as standalone SELECT commands outside
explicit transaction blocks. dolt_add stages a captured snapshot without an
implicit SQL commit. Public command APIs remain superuser-only until branch and
history authorization is implemented. PostgreSQL owns atomic object/ref updates.

Capture gathers eligible metadata and table/index roots under one snapshot with
DDL coordination. The serialized subset includes namespaces (including empty),
plain prolly tables with built-in column types, and their supported indexes.
Unsupported user objects/definitions must be rejected rather than silently omitted.
Canonical descriptors use symbolic names, ordered arrays and JSONB text encoded
as UTF-8. Physical identifiers and transaction metadata stay outside payloads.

The staged snapshot is separate from live contents. Commit without -A uses the
staged snapshot; -A stages all supported live contents. WORKING in diff means a
fresh live capture, not the last persisted working ref. Ordinary DML is not yet
wired to working refs. Concurrent version commands validate expected ref tokens;
this slice does not remove the existing table writer serialization.
