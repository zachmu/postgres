# Canonical definitions and local storage resources

Status: the resource read/allocate/rollback boundary is implemented. Qualified
working-branch SQL uses it automatically. The full SQL CREATE/DML path through
canonical definitions is **not** implemented; same-name CREATE still fails.
This is a prerequisite within the writable gate, not completion of that gate.

## Identities and responsibilities

A backend-local catalog handle identifies an immutable definition. It must not
be used as a shared lock or file identity. `relation_open`/`try_relation_open`
pin these definitions without registering shared relation locks or statistics
under their backend-local IDs. Physical relations retain their native behavior.

`pg_dolt_binding` maps (branch, logical qualified name) to a canonical schema hash
and local storage resource ID (`bindrelid`, the existing column name). The new
`pg_dolt_storage` maps that resource ID to:

* database, tablespace and file number;
* access method and table/index kind;
* the exact compatible canonical schema hash;
* whether the resource was imported from a native physical relation.

There are no names, columns, types, indexes or constraints in a resource record.
Resource IDs/locators never enter canonical hashes. Compatible branches can share
one resource. A resource is not a per-branch persisted schema projection.

`dg_resource.c` implements the boundary. It locks the stable resource ID before
reading its locator, checks the branch mapping and canonical schema under the
statement snapshot, and constructs a private descriptor combining canonical
metadata with the resource locator/lock identity. It retains that snapshot for
root selection. The cached canonical descriptor is never mutated. Resource locks
remain held to transaction end; definition pins and private smgr pins have their
own normal/error cleanup.

Qualified working scans now load through this private descriptor. Retained scans
continue to read manifest chunks directly. Plans depend on binding/resource/ref
catalog invalidation and rebind under the appropriate snapshot. A mismatched,
missing or incompatible resource raises an error rather than substituting HEAD
or a same-named physical table.

## Imported resources versus independent allocation

Capture registers existing native table/index files. Their resource ID currently
equals the native relation OID so old writers/DDL and new bound readers share a
lock identity. While native physical DDL still owns these files, opening an
imported resource validates its current locator, AM and tuple layout against the
canonical definition. That check can reject a stale binding; it cannot override
canonical metadata. Current same-name physical revocation checks also remain.
These transitional dependencies must disappear as canonical DDL takes ownership.

`dg_storage_allocate` is the new internal CREATE storage primitive. Given a
canonical prolly table/index descriptor, its schema hash and a branch, it creates
an independent permanent file and local resource row. It initializes an empty
branch-tagged prolly root through the existing buffer/generic-WAL path. It writes
no physical `pg_class`, `pg_attribute` or `pg_type` definition. The caller must
publish canonical membership and the branch binding in the same SQL transaction.
No customer-facing allocation or preparation function is installed.

Only independently allocated resources avoid the physical definition validator.
The allocator selects an unused local resource OID and avoids current physical
relation OIDs. Imported registration rejects an identity owned by an independent
allocation instead of overwriting it. The current registry writer lock serializes
resource metadata writers; scalable allocation and complete OID wraparound/lifetime
policy remain future work.

Native pending-delete machinery cleans allocation on explicit abort/savepoint
rollback; final unlink follows checkpoint cleanup. Crash-before-commit testing
found an unreachable file left behind while the resource row and binding remain
uncommitted. Resource reclamation must cover this case. Commit/restart and cold
physical standby reads pass without persisted schema copies. This does not prove
logical dump/restore, vacuum/reclamation or every physical maintenance path.

## Evidence and boundaries

`test_prolly/t/018_storage_resources.pl` exercises production qualified scans:
shared resource/independent branch roots, internal ACLs, stable lock conflicts,
absence of backend-local relation locks, schema/AM mismatch rejection, MVCC
resource visibility, native CREATE metadata rollback/savepoints, recovery and
standby reads.

`test_doltgres/t/003_storage_allocation.pl` uses a separate test-only C module to
call the internal allocator. It publishes a binding to an **existing** canonical
definition with internal catalog writes; this is not a new SQL CREATE
implementation. It verifies no physical definition rows, real table/index files,
empty-root reads through ordinary qualified SQL, uncommitted publication isolation,
abort/savepoints, prepared-plan rebinding, old-snapshot retention, WAL recovery and
standby. Its helper is kept separate from the old projection fixture's parser hook.

General native DML still uses the existing physical bridge. Writes through
unbound canonical handles remain rejected. Native constraints/index maintenance,
DDL dependency traversal, ordinary branch catalog activation and schema publication
are not completed by this read/initialization API. Existing table-wide writer
serialization and broad DDL guards remain.

## Immediate continuation

1. Bind native writable descriptors to stable resources with explicit branch,
   schema, snapshot and write authority. Adapt table/index AM consumers and their
   lock identities without mutating shared immutable definition descriptors.
2. Construct provisional canonical CREATE definitions, row types, constraint/index
   definitions and dependency edges. Publish canonical working membership, branch
   bindings and allocated resources together under PostgreSQL transaction rules.
3. Route ordinary name lookup/catalog introspection and capture through that
   provider. Capture/checkout still enumerate physical definitions today and
   cannot be used unchanged for independently allocated canonical relations.
4. Pass the preserved same-name CREATE suite and two-session open-DDL/commit/
   abort/savepoint scenarios. Reclaim abandoned resources and audit maintenance,
   prepared-plan lifecycle, permissions and snapshot behavior.

No private namespaces or persisted historical schema copies are proposed. Root
plan documents were not edited in this implementation pass; this file records
implementation evidence and the remaining integration boundaries.
