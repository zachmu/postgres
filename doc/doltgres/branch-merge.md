# Branch checkout and data merge slice

This slice adds checkout, merge-base discovery, fast-forward/no-op and three-way
data merge over retained commit objects. PostgreSQL owns the atomic installation
of live data, rebuilt indexes, history objects, refs and pending merge metadata.

## Explicit compatibility decisions

A data merge conflict is a successful SQL function invocation with `conflicts=1`
in the Dolt-compatible `(hash, fast_forward, conflicts, message)` result. It must
not raise ERROR and abort the transaction. Persist the conflict evidence and
pending parents so the user can inspect, resolve, stage and commit. This
intentionally differs from the current Doltgres conflict-error behavior and its
skipped tests. Invalid arguments, unsupported operations and infrastructure
failures remain errors. Unresolved conflicts prevent a history commit.

Schema merges are excluded: differing namespace/table/index definitions in the
base/ours/theirs inputs must be rejected before installation. Primary-key rows
must merge by logical key, not row-content hash. Keyless tables require occurrence
count arithmetic. Indexes derive from the resulting table contents and must pass
uniqueness validation; they are not independently merged.

The current standalone SELECT / SQL transaction boundary remains in force for
checkout and merge. Conflict metadata and pending merge state must survive restart
and abort/rollback consistently with live roots. Do not add a second publication
manifest or bypass PostgreSQL WAL.
