# Branch-aware catalog implementation contract

Status: work in progress, not a completed branch-aware catalog. The native lookup
boundary and synthetic-provider tests are implemented. Production canonical
schema decoding, automatic context binding, DDL publication, and same-name CREATE
are not yet connected. The sections below describe the required end state.

This work replaces the withdrawn private-namespace approach. Canonical versioned
schema definitions are authoritative. No persisted physical schema copy is created
for a branch or revision. Local storage metadata supplies file/root locators only.

## Context and identity

A catalog view is an immutable database schema root plus working-branch or retained
revision selection and the SQL snapshot used to resolve it. Each object reference
is bound to that view. Looking up a name uses the caller's view; following an object
handle uses the view encoded by the handle, regardless of the session's subsequent
branch selection. Two references in one statement can therefore use incompatible
schemas. Runtime handles are backend-local and are not canonical object identities.
No handle or physical allocation identifier enters canonical schema bytes.

Runtime handles are allocated for live contexts and have explicit lifetime owners.
Plans, portals, tuple descriptors and executor nodes must retain their contexts.
Cross-backend execution must transport canonical view/object references and bind
locally, never assume that a backend-local OID has the same meaning elsewhere.
Unsupported execution paths must reject the operation before accessing storage.

## A single lookup boundary

The same provider serves native catalog scans, syscache, relcache/type consumers,
and SQL catalog introspection. Cache keys include context for name-based lookups;
object handles select immutable context for OID-based lookups. Native catalog rows
for versioned objects are hidden, never a fallback. PostgreSQL-owned/bootstrap
objects retain their existing physical implementation.

Catalog scans must work consistently with index scans disabled, cache cold or warm,
prepared plans, old SQL snapshots, explicit revision qualification, read-only
transactions and standbys. Context-free unsupported catalog access must fail
explicitly. Dependency traversal follows bound identities, not coincidentally
matching physical OIDs.

## DDL and storage

Native DDL may construct provisional catalog tuples in transaction-local provider
state. Publication converts supported definitions to canonical schemas and updates
working membership atomically with storage allocation/root publication. It must
not persist pg_class/pg_attribute/pg_type copies as the schema authority. Abort and
subtransaction rollback discard provisional definitions and allocations together.

Storage locks and WAL refer to stable local storage resources, not backend-local
catalog handles. Shared resource locators do not imply shared schemas or branches.
A bound relation's schema, branch/revision and write authority are checked before
storage access. Historical reads need no writable catalog or preparation API.

## Acceptance

Retain the same-name Doltgres scenarios unchanged. Validate cold/warm native caches,
logical pg_catalog and information_schema results, revision-qualified catalog
joins, row types and dependencies, prepared statements, old snapshots, DML and
constraint indexes, DDL abort/savepoints, crash recovery, read-only and standby
reads. Run existing PostgreSQL regression/isolation tests unaltered. Report exact
supported object/DDL forms and reject unsupported forms; do not confuse passing a
lookup fixture with completing the architecture.

## Current implementation evidence and gaps

`catalog/catalogview.c` supplies immutable native tuples to syscache, ordinary and
ordered system scans, and sequential SQL catalog scans. Revision-bound OIDs select
their view in syscache, relcache and typcache. A borrowed cache tuple/list or scan
pins the supplying view. Relation descriptors pin their view until destruction;
native type-cache entries currently pin it for the backend lifetime. That last
policy prevents dangling OIDs but is not an acceptable final memory-reclamation
policy for a session visiting many schema revisions.

The server fixture presents two identically named composite types whose first
columns have different types. Repeated cross-view cache/descriptor lookups and SQL
catalog scans agree. Ordered scans use native index comparison support without
reading physical index pages. Physical catalog writes and unsupported direct heap
access are rejected while the supplied catalog is active. Borrowed-tuple lifetime
and retired-handle failure are tested separately.

This is not yet production branch resolution. The preserved Doltgres same-name
suite still fails on the old CREATE path. No private namespaces were reintroduced.
Before activation, the provider must hide all versioned physical definitions,
decode complete canonical metadata, bind storage locks/root access independently
of runtime OIDs, publish provisional DDL transactionally, and pin/rebind plan and
portal contexts. Per-reference catalog queries, dependency traversal, parallel
transport, error cleanup, snapshot invalidation, and reclamation require further
integration tests. Passing the fixture must not be reported as satisfying those
requirements.
