# Branch-aware catalog implementation contract

Status: the first integrated canonical read gate is implemented. Explicit
revision-qualified table/catalog references bind production canonical definitions
to native catalog views, relation descriptors and row types. Ordinary writable
relation binding, provisional DDL publication and same-name CREATE still require
integration. The sections below describe the required end state.

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

## Initial infrastructure evidence and gaps (before production integration)

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


## Production canonical read integration — 2026-10-22

`access/dolt_prolly/dg_provider.inc` decodes a retained canonical database root into
immutable native catalog tuples. Namespaces, relations, attributes, composite row
types, comments, supported indexes/constraints and their dependency edges come from
canonical member definitions. Bootstrap definitions and global roles still come
from PostgreSQL. Live user relation definitions are excluded from the provided
catalogs. Local role names resolve to local role OIDs; this does not put those OIDs
into canonical schema bytes.

`dg_catalog_bind` resolves each explicit revision independently, records the
canonical database identity on its RTE and obtains the table descriptor through
native relcache using a view-bound handle. Whole-row expressions carry the bound
composite type. `pg_catalog.pg_class AS OF ...` and the other supported catalog
references use the same provider through a custom scan. Explicit revisioned
database qualifiers select this path automatically too. No preparation function,
persisted schema projection or private runtime namespace is involved.

SQL functions with OID arguments recover the object's view for nested catalog
queries. Function cache keys include the view identity; scalar and set-function
inlining must preserve that boundary. Switching context during an active SQL SRF
is rejected. Ordinary tuple reads may sign-extend OID Datums: context detection
normalizes the OID rather than rejecting those values as non-OIDs.

Canonical descriptors have no physical locator. Retained row scans use the existing
canonical tree/chunk path; compatible working-branch scans still use existing
local storage bindings. Direct prolly AM storage access and physical-size helpers
reject definition-only handles. This is not yet a complete physical-access audit.

Native TAP 017 covers incompatible revisions in one statement; whole-row fields
and JSON; relation/type/namespace/attribute/dependency/index joins; constraint
and comment helpers; alternating function contexts; prepared plans; cursors;
read-only readers; restart; and cold standby reconstruction. Replacement of the
live table with an unrelated definition cannot override the canonical descriptor.
The test additionally checks explicit rejection of unsupported catalogs and
physical-size requests. Existing permission policy still checks historical grants
and current revocations on same-named live objects.

### Remaining boundaries

* Default-branch ordinary catalog scans and writable relation binding are not
  switched wholesale to canonical views. Same-name CREATE acceptance still fails.
* The supplied catalogs are pg_namespace, pg_class, pg_attribute, pg_type,
  pg_description, pg_depend, pg_index, pg_constraint and bootstrap pg_cast.
  Explicit revision scans of other catalogs reject. This is not complete
  information_schema coverage or versioning of functions/types/roles.
* The existing capture envelope still excludes defaults, generated/dropped
  columns, custom types, triggers and non-unique constraints. Index decoding
  supports simple prolly keys/INCLUDE, not expression/partial indexes or options.
  Dependency edges cover this supported subset, not arbitrary DDL traversal.
* Composite array types are not synthesized; canonical row-type typarray is zero.
  SQL helper context inference currently recognizes OID parameters, not every
  alias type or every language handler.
* Providers and native type caches retain views for backend lifetime. Bounded
  reclamation, role-change invalidation and cross-backend/parallel handle transport
  remain unresolved production concerns. Explicit historical scans are not
  parallel-safe.
* The next writable gate needs transactional provisional definitions, stable
  storage resource binding independent of runtime handles, branch-local dependency
  and permission checks, and ordinary branch catalog activation. Use PostgreSQL
  transaction/rollback semantics; test other-branch sessions during open DDL,
  commit, abort, savepoint rollback and recovery. No private namespace workaround.

The report `reports/canonical-catalog-provider.html` in the workspace records the
exact verification selection and known failure. The first read gate is evidence
for continuing this architecture, not a claim that branch-aware DDL is complete.
