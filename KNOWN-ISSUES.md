# Deferred defects

User-directed deferrals from func-review.txt (2026-10-23). These remain defects,
not supported semantics. Address independently with regression tests.

* **Prepared dynamic diff result layouts:** preparing dolt_diff, changing the
  table schema and executing the plan can crash the backend. Bind result schema
  dependencies and validate runtime descriptors before writing tuple arrays.
* **Analysis-only dolt_ignore creation:** EXPLAIN/PREPARE of the first INSERT can
  create the table during parsing. Move lazy creation to execution.
* **dolt_query_diff row equality:** unescaped text separators permit distinct
  rows to compare equal. Replace with typed length-delimited encoding.

The SQL reproductions are in /workspace/reports/rewrite-dolt-functions-*.sql.
