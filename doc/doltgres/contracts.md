# Doltgres phase 0 contracts

Status: executable design contracts for phase 1, not a production table AM.
Base: PostgreSQL REL_18_6. Authority: workspace PLAN.md and design-questions.txt.

## Representation and identity

* A database content root contains symbolic schema definitions and table/index
  roots. Index contents are versioned. Commit ancestry, branch names, runtime
  OIDs, XIDs, command IDs, lock state, row handles, allocator state, and statistics
  are outside this root. Different history nodes can share one content root.
* Canonical references are qualified names plus overload/type signatures where
  applicable. Rename is an explicit reference rewrite, not a change to a hidden
  history-dependent ID. Built-in runtime OIDs remain unchanged.
* Runtime OIDs are stable local handles for a branch's execution objects. They
  may be persisted but never serialized into canonical references. Different
  simultaneous branch definitions need distinct execution identities. Clone
  reconstructs mappings. Allocation is not truncation of a content hash.
* Runtime occurrence/version handles distinguish duplicate rows and updates.
  They are retained while snapshots, index fetches, portals or prepared work
  can reference them. TID adaptation is a phase 2 obligation, not yet selected.
* Prototype keyless candidate: canonical row bytes -> multiplicity. This is an
  experiment, not a requirement to adopt Dolt's representation. Its criterion
  is equal multiset roots despite different occurrence handle allocation.

## Canonical storage experiments and current implementation

The original Python schema/value/table oracle has been replaced by a standalone
C implementation in `src/doltgres/prolly`. Its versioned node format is explicit
big-endian data with separate key/value regions and offset arrays. Leaves store
opaque tuple bytes; internal values contain child hashes and entry counts.
SHA-256 addresses immutable nodes. Branch, diff, three-way merge, ordered scans,
and incremental edits have native tests.

The C tuple helper supplies a null bitmap and attribute offsets, excluding heap
MVCC metadata and runtime identities. Type interpretation is external, analogous
to TupleDesc. It includes a sortable int64 key encoder; arbitrary SQL numeric,
collation, schema-reference and operator-class encodings are not implemented.
These remain requirements for the integration layer, not claims about raw
byte-map semantics. The previous Python encodings were experiments and are not
compatible with the C node format. See the C README for exact limits and layout.

Equality of the generic map means equality of canonical input bytes. At the SQL
boundary, callers must ensure SQL-equal values have canonical encodings under
fixed type/collation rules. This preserves the original project requirement;
it does not imply that hashing arbitrary PostgreSQL values satisfies it. NULL,
SQL duplicate multiplicity and symbolic references need explicit higher-level
representations. No runtime OID, XID, padding or allocator state may enter them.

Native tests compare fresh construction with incremental mutation, branching,
merging, deletion, empty-value handling and old-root persistence. The former
keyless multiplicity policy and canonical table/index wrapper were removed with
the Python prototype; their eventual representation remains open. The current
request prioritizes general C tree operations before PostgreSQL API integration.

## Snapshots and publication

READ COMMITTED obtains a statement view. REPEATABLE READ/SERIALIZABLE retain a
transaction view. A snapshot pins a consistent set of branch roots; a qualified
branch name is not permission to bypass visibility. Own changes additionally
obey command IDs and subtransaction state. Checkout and prepared-plan rebinding
policy must be specified before enabling mutable session branch selection.

The publication protocol is:

1. Stage immutable chunk contents and tentative multi-ref updates under an XID.
2. WAL must make all referenced new content recoverable before COMMIT can be
   durable. In v1 model records contain the necessary chunk bytes.
3. PREPARE retains the proposal and content but publishes nothing.
4. Durable COMMIT determines one all-or-nothing update across the proposal's
   refs. Recovery can finish publication idempotently. ABORT publishes nothing.
5. Runtime readers consult an MVCC history of decisions, not only one latest
   manifest pointer. Retention includes snapshots and unresolved prepared work.

A precommit hook alone is not a durable publication protocol. A postcommit hook
alone cannot close a crash gap. Old-root validation in the model only prevents
lost manifest updates; it is NOT PostgreSQL row conflict handling or isolation.
The model does not implement WAL IO, fsync, SSI, command visibility, locks or
logical decoding. Production integration belongs to phases 2–3.

## Compatibility matrix

| Feature | Requirement | Phase 1 experiment |
| --- | --- | --- |
| Heap replacement | PostgreSQL table-AM semantics | Native C byte-map/tuple prototype; no table AM |
| User btree | Actual prolly index trees, unique/order/include semantics | Generic keyed maps; no SQL index AM replacement yet |
| Other user index AMs | Explicit rejection in first product draft | Existing server unchanged until phase 2 |
| Catalogs | Divergent branch definitions in one query | Test projection with real relation/type OIDs |
| OIDs | Built-ins unchanged, stable runtime mappings desirable | Exclude from hashes; test distinct branch objects |
| MVCC/locking | PostgreSQL semantics, separate metadata | Snapshot/publication contract model plus real SQL catalog tests |
| WAL | Recovery authority for chunks and decisions | Crash-prefix model; no custom rmgr yet |
| PREPARE/savepoints | Preserve PostgreSQL behavior | Publication model / real catalog DDL rollback |
| Implicit commit | Standalone top-level form initially | Not enabled by phase 1 |
| Sequences | Shared local allocator, versioned definition | Definition-only canonical state; bootstrap deferred |
| Replication | WAL and logical replication to fork instances | Explicit later integration, not provided by the model |
| Remotes/GC | Content/history transport; retain all pinned roots | Root reachability oracle only |

## Catalog candidates and decision gate

A branch context layered over one cached relation OID cannot describe two
incompatible tuple descriptors in one query. Direct branch-aware catalogs would
need new identities or changes to relcache, syscache, typcache, dependencies,
lock tags and plan invalidation. Evaluate a projection with distinct OIDs first,
without assuming it is a complete solution for symbolic DDL or arbitrary types.

Gate: execute a query over divergent branches, prepared plan invalidation, and
DDL rollback on PostgreSQL 18.6; inventory touched interfaces. Prefer the
smallest demonstrated implementation. Missing production features must remain
listed explicitly. Phase 1 does not claim full branch-aware pg_catalog queries.
