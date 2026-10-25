# Deferred defects

User-directed deferrals from func-review.txt (2026-10-23). These remain defects,
not supported semantics. Address independently with regression tests.

* **Prepared dynamic diff result layouts:** preparing dolt_diff, changing the
  table schema and executing the plan can crash the backend. Bind result schema
  dependencies and validate runtime descriptors before writing tuple arrays.
* **Analysis-only dolt_ignore creation:** EXPLAIN/PREPARE of the first INSERT can
  create the table during parsing. The new lazy docs/tests/query-catalog table
  adapters use the same mechanism. Move lazy creation to execution.
* **dolt_query_diff row equality:** unescaped text separators permit distinct
  rows to compare equal. Replace with typed length-delimited encoding.

The SQL reproductions are in /workspace/reports/rewrite-dolt-functions-*.sql.

* **Forced branch deletion and old sessions:** ordinary deletion rejects active
  sessions, and writes after a visible deletion fail. A transaction retaining an
  older snapshot, or a session surviving forced deletion and same-name recreation,
  still needs a commit-time authority check. This is the existing-session edge
  explicitly allowed to defer in func-review.txt. Branch creation itself now
  overwrites working refs and table roots, so it does not resurrect old contents.
