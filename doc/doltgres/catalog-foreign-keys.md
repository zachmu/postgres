# Revision-bound foreign keys and triggers

Foreign keys are canonical schema members. `dg_foreign_codec.inc` adds the
supporting index's qualified name and the three native comparison-operator
signature arrays to each foreign-key constraint's existing name/SQL definition.
Runtime relation, constraint, index and trigger OIDs are not serialized.
`dg_foreign_provider.inc` reconstructs `pg_constraint`, the four internal RI
`pg_trigger` rows, and dependency edges in the selected immutable catalog view.
SQL catalog reads and native caches see those same rows.

Native ALTER TABLE creates, validates, changes deferrability and drops the
constraints in a provisional view. The canonical publisher serializes the
resulting graph. Inline CREATE REFERENCES uses that same ALTER controller before
the SQL transaction commits. Native dependency closure handles dependent
constraints when a referenced table or column is dropped with CASCADE.
A new optional table-AM `relation_finish_rewrite` callback marks rewritten
provisional contents ready for the final native FK validation pass.

## Execution context

A trigger executes with its owning relation's catalog view and explicit branch.
Deferred events retain the schema view until the transaction releases them and
store the originating branch separately from the runtime relation handle.
Firing resolves current snapshot-visible data on that branch without replacing
the retained definition. A mismatched definition is rejected. Pending-event
checks compare branch and logical relation identity, including when runtime
handles differ between views. Rollback discards queued subtransaction events.

RI cached query plans include a backend-local branch token in addition to their
constraint handle. Two branches can share an identical schema handle but have
different data. An execution scope prevents nested RI queries from accidentally
binding to a historical data root or the session's unrelated current branch.
Native heap trigger and tuple-lock behavior remains in place.

## Concurrency and publication

Prolly RI checks take the existing branch/resource writer lock instead of native
heap key-share tuple locks. Those locks survive until transaction end. This is
safe but coarse: competing writes/checks on one branch can serialize, while
independent branches remain concurrent. Under a fixed snapshot, a changed
referenced content root yields SQLSTATE 40001; this can conservatively reject
unrelated row changes. SERIALIZABLE prolly writes remain unsupported.

Three-way root combinations, conflict-resolution installs and partial table
restores bypass ordinary DML triggers. They therefore validate the resulting FK
graph after installing all selected roots and before publishing the transaction.
An invalid result rolls back atomically with a foreign-key error. This is not yet
Dolt's retained foreign-key constraint-violation state. The check conservatively
includes NOT VALID constraints, so recombination can reject pre-existing orphans.
Complete retained-database restores and fast-forwards preserve an existing root
and do not revalidate it.

## Scope and validation

Supported: ordinary nonpartitioned enforced FKs, inline REFERENCES, ADD/DROP,
NOT VALID/VALIDATE, deferrability, native referential actions, indexed type
rewrites, dependency deletion, branch isolation, and historical introspection.
Arbitrary user-defined trigger serialization, temporal/partitioned/unenforced
FKs, schema merges, retained FK violation records and row-level locking remain
outside this checkpoint. Existing type/index codec limitations still apply.

`test_doltgres/019_foreign_keys` covers enforcement, deferred events, permissions,
rewrites, recovery, two-session branch independence, concurrent parent deletion,
merge and partial-restore integrity. The Doltgres catalog-context gate includes
20 original FK scenarios and one branch scenario; exclusions are explicit in the
script. The PostgreSQL regression, isolation and PL/pgSQL suites remain unchanged.

Capture and working-tree schema diff now share the same schema encoder, including
FK metadata. Schema JSON is compared by value rather than whitespace/property
ordering; unchanged schemas no longer produce spurious diff or patch entries.
