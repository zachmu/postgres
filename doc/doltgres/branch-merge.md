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

## Branch binding and future cross-branch queries

This milestone requires independent session branch views. The selected session
branch supplies the default binding; it must not become the permanent identity
of every reference to a relation.

Ultimately a single query must be able to reference the same table on multiple
branches or retained commits, including joining those references. Resolve each
table reference to an explicit revision binding, carry that binding into its
plan and scan state, and select its schema and data roots under the appropriate
snapshot. Separate aliases of the same relation may have different bindings.
Index scans, tuple fetches and rescans must retain the binding of their own table
reference. A shared relcache entry or a backend-global branch variable alone
cannot represent this distinction. Cached plans must preserve explicit bindings
and revalidate default bindings when checkout changes the session context.

Cross-branch queries and revision-specific schema projection are outside this
milestone. The current relation lookup boundary is a transitional seam, not a
complete implementation of per-reference binding. Future writes must likewise
identify their target working set explicitly; retained commit views are read-only.
