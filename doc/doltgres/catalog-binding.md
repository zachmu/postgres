# Native versioned catalog provider (in development)

Historical queries must work on first access, without preparation, persistent
catalog writes, or elevated user privileges. Explicit and hidden persistent
materialization are not the query mechanism. The materializer and projection
catalog have been removed. The earlier experiment's report is historical evidence,
not the current architecture contract.

The replacement introduces a native versioned range-table entry containing a
canonical table descriptor address and an in-memory column descriptor. A native
scan reads retained prolly chunks lazily. It does not allocate relation OIDs,
create namespaces/tables, copy historical data into new relations, or rebuild
indexes to make a query possible. Ordinary PostgreSQL access methods are unchanged.

The initial implementation targets retained table reads; further test results,
limits, branch working-set binding and integration details will be recorded before
this milestone is merged. No optional administrative function is a prerequisite
for correct query execution.
