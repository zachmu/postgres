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

## Canonical format v1 experiment

SHA-256 with domain-separated, length-framed inputs. Integers use big-endian
signed-order-preserving encoding, text uses UTF-8 with C-collation equality,
booleans use one byte, bytea is literal bytes, and NULL has its own tag. Numeric
encoding removes redundant decimal zeros and canonicalizes zero; NaN and
infinities have explicit codes. No pickle, native struct padding, OID, TOAST
pointer, hash-map iteration order, or host-endian scalar enters the format.

The first experiment supports bool, int8, text (C collation), bytea, and numeric.
Unsupported type/collation encodings fail explicitly. NULL and duplicate
multiplicity are explicit. Format, encoding, and comparator identities belong
in the schema representation. Byte ordering of the generic tree is separate
from SQL operator-class ordering: prototype framed text/numeric/composite encodings are canonical but
are NOT general ordered PostgreSQL index keys. Phase 2 must implement ordered
encodings/comparators for every advertised index capability.

Equality means SQL equality for supported values under a fixed type/collation.
SQL-equal values can have different display forms; v1 intentionally normalizes
those forms. Extending this to nondeterministic collations, domains, custom
operator classes, composite references and observable text representations
requires explicit policies, not a fallback to arbitrary binary output.

Tests must compare fresh construction with incremental edits, reversed and
random insertion histories, deletes/reinserts, duplicate multiplicities,
schema changes, index contents, and independently allocated runtime identities.
Persistent roots must remain readable after later mutations.

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
| Heap replacement | PostgreSQL table-AM semantics | Canonical storage oracle only |
| User btree | Actual prolly index trees, unique/order/include semantics | Canonical index contents; no AM replacement yet |
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
