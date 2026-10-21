# Manifest foundation: publication contract

This is the internal foundation underneath the
[first version-control command slice](version-commands.md).
Mutable refs and immutable objects live in per-database heap catalogs. Existing
heap MVCC, row/index locking, WAL, savepoints and two-phase commit are authoritative.
No external manifest file, postcommit rename or independent commit decision exists.

## Objects and identity

Objects form an immutable DAG. SHA-256 covers a versioned, length-delimited
encoding of kind, payload and ordered child hashes. Children must already be
visible in this transaction. Objects contain no runtime OIDs or transaction IDs
unless a privileged internal caller incorrectly puts them in its payload.
The generic payload API is an internal serialization boundary, not an automatic
PostgreSQL schema serializer or an admission check for arbitrary SQL values.
Schema descriptors and database member mappings must use canonical symbolic
names and formats supplied by the eventual schema integration layer.

Kinds are chunk, schema, table, index, database, commit, namespace and tag. A table/index names
its schema and content chunk; a database names namespace/table/index descriptors; a commit
names a database state followed by zero or more parent commits. Chunk objects may
link other chunks. Namespace objects have no children; annotated tag objects name
a commit. Schema payloads remain opaque to the generic object API; the command
layer now supplies a limited canonical schema format.
A raw chunk's object address includes its envelope and differs from the prolly
node hash. Snapshot export preserves the original node bytes and a mapping to
node hashes; these namespaces must not be confused.

All linked objects are retained, independently of source relation lifetime.
There is initially no deletion or GC API. Heap VACUUM/freezing handles metadata
XIDs; historical object reachability and reclamation are separate future work.

## Ref state transitions

A read returns (target, revision token) under the caller's SQL snapshot. Creation
requires an absent ref; update/delete requires the exact previously read token.
Every create/update generates a fresh random 256-bit token, avoiding ABA after
move-back or delete/recreate. A stale token fails with serialization_failure;
concurrent creation uses the unique index and may fail with unique_violation.
Names are database-local, with heads/, tags/, working/ and staged/ namespaces.
Heads point to commits; tags permit annotated tag objects or legacy direct commit
targets; working/staged refs point to database states. The generic CAS API can
move tags; the public command slice only supports creation.

SQL UPDATE/DELETE performs waiting and predicate rechecking. Read Committed can
see newer committed versions on subsequent statements; Repeatable Read retains
its snapshot and PostgreSQL rejects conflicting updates. Multiple ref moves and
ordinary heap/catalog/prolly changes share one transaction outcome. Callers that
move several refs must order names consistently or handle PostgreSQL deadlocks.
There is no hidden SQL commit. Command boundaries expose own writes; rollback to
savepoint discards only changes after that savepoint. Prepared transactions retain
heap/index locks and remain invisible until COMMIT PREPARED.

## Publication and recovery

1. Build immutable contents and descriptors from one eligible SQL snapshot plus
   the transaction's own command-visible changes.
2. Persist all required objects through heap WAL; children precede parents.
3. Perform expected-token ref changes in the same SQL transaction.
4. PostgreSQL commit makes the complete transaction visible. WAL recovery uses
   that same outcome. Abort leaves no visible new objects or ref versions.

A heap ref tuple being visible does NOT establish visibility of arbitrary
transactions incorporated in its target. Explicit version-control commits are
immutable snapshots assembled from an eligible view, not a universal replacement
for live SQL table state. Future row publication must select/apply snapshot-visible
row deltas, or prove an aggregate root's eligibility. It must not blindly publish
an old private root, nor update one shared working-ref tuple on every row write.
The concrete disjoint-writer algorithm remains the row-level milestone's work.

## Boundaries

Functions are privileged internal test/building APIs. Ordinary AMs and table
write paths are unchanged. The command layer provides limited automatic database capture and version-control
functions. Branch schema resolution, logical dump/restore of history, GC and a
ref authorization model remain future work. Physical backups include catalogs.
Fresh initdb is required. Snapshot export initially copies the canonical tree
only; executor handle trees remain runtime state and are not historical content.

## Implemented storage and API

`pg_catalog.pg_dolt_object` stores `(objhash bytea, objkind text,
objpayload bytea, objchildren bytea[])`, with a unique hash index and heap TOAST.
`pg_catalog.pg_dolt_ref` stores `(refname text, reftarget bytea, reftoken bytea)`,
with a unique name index. Both catalogs are per database. Catalog text columns
use C collation. There are no syscache entries: reads deliberately use SQL/SPI
snapshots, not catalog-cache visibility shortcuts. Superuser access is required
in C as well as by the initial ACLs. Direct superuser catalog edits are outside
the API's invariants.

The object hash input is exactly:

```
"DGMOBJ01" | kind:u32 | child_count:u32 | payload_length:u32
            | child_hash[0]:32 ... child_hash[n-1]:32 | payload
```

Integers are big-endian; kind IDs 0–5 are chunk, schema, table, index, database,
commit respectively; IDs 6 and 7 are namespace and tag. Ordered children and payload bytes are significant. For
example, different commit metadata or parent order changes the commit hash even
when the database target is identical. The payload format is caller-owned and
must be versioned; no semantic normalization or symbolic schema encoding is
inferred by this primitive. No SQL OIDs, XIDs or heap tuple headers are added to
this encoding. Ref revision tokens are random coordination metadata and never
enter canonical content hashes.

| Internal SQL function | Result |
|---|---|
| `dolt_manifest_put(text, bytea, bytea[])` | Validate child kinds and store/deduplicate an immutable object; return its hash. |
| `dolt_manifest_get(bytea)` | Return `(kind, payload, children)` under the active SQL snapshot; absent objects yield null fields. |
| `dolt_manifest_read(text)` | Return `(target, token)` under the active SQL snapshot; absent refs yield null fields. |
| `dolt_manifest_cas(text, bytea, bytea)` | Create with null expected token, move with a matching token, or delete with null target; return the new token, or null for deletion. |
| `dolt_manifest_export(regclass)` | Copy the snapshot-visible canonical table/index tree into chunk objects; return the envelope hash of the exported root. |

Export walks only reachable nodes, preserves their original bytes and child
order, and excludes runtime handle maps and unrelated historical nodes. It does
not create a schema/table/database descriptor or a ref. Full database capture
must collect all descriptors/trees under a consistent snapshot, not separate
Read Committed statements that can observe different commits.

Object insertion first checks for an existing visible object, then takes a
transaction-duration lock derived from its hash on a miss, rechecks and inserts.
Concurrent creation of the same new object can wait; Read Committed deduplicates
after the creator commits. Repeatable Read cannot adopt an object outside its
snapshot and may raise unique_violation, requiring a whole-transaction retry.
Hash-lock collisions cause extra serialization, not incorrect identity. Multiple
object insertions can deadlock and rely on PostgreSQL's detector; upload callers
must support transaction retries. This is a simple metadata implementation, not
the final high-throughput chunk-ingestion path for concurrent row writers.

## Validation and remaining integration

`src/test/modules/test_prolly/t/006_manifest.pl` runs real servers and sessions.
It checks independent hashing, type/target validation, duplicate creation, ABA,
stale CAS, wait/commit and wait/abort, object deduplication, old snapshots,
own writes, savepoints, multi-ref/mixed heap/prolly prepared commit, recovered
prepared locks, abort, immediate crash/restart, TOAST, source DROP/TRUNCATE,
physical backup/restore, permissions and per-database separation.

This completes the manifest foundation portion of steps 1–2. It does not complete
row-level plan milestone 1: the live row-delta/base-root eligibility algorithm
and its lifetime/compaction proof still need implementation-specific resolution.
Do that before connecting ordinary DML to working-set publication. There is no
single database/branch writer lock in this foundation, but explicit updates to
the same ref correctly serialize until transaction end.

History is included in physical backups. `pg_dump` does not export these system
catalog contents: a logical dump currently saves ordinary SQL objects/data and
omits manifest history. Do not use it as a version-history backup. A future
logical-history format/importer and upgrade strategy are required.
