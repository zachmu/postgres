# Manifest foundation: publication contract

This is an internal foundation, not the user-facing dolt_commit/checkout API.
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

Kinds are chunk, schema, table, index, database and commit. A table/index names
its schema and content chunk; a database names table/index descriptors; a commit
names a database state followed by zero or more parent commits. Chunk objects may
link other chunks. Schema objects are opaque canonical descriptors at this stage.
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
Names are database-local, with heads/, tags/ and working/ namespaces. Heads/tags
point to commits; working refs point to database states. Tags are mutable only
through the same explicit compare-and-swap protocol; immutable-tag policy is a
later user API decision.

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
write paths are unchanged. No branch schema resolution, automatic database
capture, logical dump/restore of history, GC, ref authorization model or user
version-control command semantics are claimed. Physical backups include catalogs.
Fresh initdb is required. Snapshot export initially copies the canonical tree
only; executor handle trees remain runtime state and are not historical content.
