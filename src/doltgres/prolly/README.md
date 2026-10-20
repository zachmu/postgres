# Standalone C prolly-tree prototype

This replaces the Python storage prototype. It is an actual C library with a
native test executable, independent of PostgreSQL backend APIs. The separate
Python transaction-publication model in `src/test/doltgres` remains a design
specification, not a storage implementation.

## Build and test

From the repository root:

```
make -C src/doltgres/prolly check
make -C src/doltgres/prolly sanitize
```

Dependencies: C11 compiler, make, pkg-config, OpenSSL libcrypto development
headers. No configured PostgreSQL build, server, Python, SQL extension, or
PostgreSQL headers are required. `make all` builds `libprolly.a`; include
`prolly.h` and link libcrypto. The sanitizer target builds with AddressSanitizer
and UndefinedBehaviorSanitizer and cleans those artifacts afterward.

The source follows PostgreSQL C formatting, including pgindent. The standalone
prototype uses malloc/free and explicit status returns. PostgreSQL memory
contexts, error handling and pg_cryptohash can be adapted at integration time.

## Reference and algorithm choices

Reference: Dolt checkout `72e0a00a1a33f5ae3fd8e24a4c2fbc1499967edd`, especially:

- `go/store/prolly/tree/node.go`: packed key/value access, child hashes and
  subtree counts.
- `node_splitter.go`, `chunker.go`: deterministic, content-defined boundaries
  with level-dependent splitting and shared immutable chunks.
- `node_cursor.go`: ordered traversal and seeking using internal keys.
- `three_way_differ.go`: distinguish one-sided, convergent and conflicting
  changes relative to a base.

This is a new C implementation of those architectural ideas, not a port of
Dolt's FlatBuffers wire format or a copy of its Go code. It uses SHA-256 chunk
addresses, explicit big-endian directories, and its own key-local boundary
fingerprint. The prototype is neither Dolt format-compatible nor a frozen
production storage format.

## Data model and API

The generic map has unique, unsigned lexicographic byte keys and opaque byte
values. Bulk entries and edit batches must be strictly sorted, with no duplicate
keys. Inputs are checked before mutation; deletion is explicit and is distinct
from storing an empty value. Empty keys and arbitrary binary values are legal.

| API | Behavior |
| --- | --- |
| `pl_build` | Canonical bulk construction from ordered entries |
| `pl_apply` | Ordered insert/update/delete batch, returning a new root |
| `pl_branch` | O(1) root-handle copy, no node copies |
| `pl_get` | Binary-search internal separator keys and leaf offsets |
| `pl_scan` | Ordered half-open range scan using a bounded-depth cursor |
| `pl_diff` | Ordered insert/delete/update callbacks; skip identical subtrees |
| `pl_merge` | Three-way merge relative to a base; optional row conflict resolver |
| `pl_tuple_pack/get` | Schema-driven null bitmap, offsets and attribute payloads |
| `pl_store_export/import` | Immutable serialized chunks; import verifies SHA-256 and local encoding |
| `pl_open/validate` | Validate reachable child hashes, levels, key ranges and counts |

Branch handles share store ownership; the caller must retain the store until
all handles and borrowed slices are finished. Different branches remain
independently editable. A successful edit replaces only the caller's output
handle; old roots remain readable. Functions returning a tree leave that output
unchanged on failure. Failed work may leave unreachable immutable chunks in the
store, to be reclaimed by a future retention/GC layer.

Lookup and scan values are borrowed, immutable slices valid until store_free.
Input slices need live only through the operation; written nodes copy them.
Diff callbacks may stop with PL_STOP or propagate another error. A resolver's
output bytes must be valid when it returns and are copied immediately, so an
argument-owned scratch buffer may be reused on the next callback. Returning a
pointer into the resolver's expired local stack is not valid C ownership.
`pl_tuple_pack` allocates a buffer that the caller must free.

Diff works across stores. Merge currently requires all three roots in the same
store; import remote chunks and open/validate their roots first. Root copies are
not named branch refs, transactions, or a durable publication operation. The
store is single-threaded and must be externally synchronized if shared.

## Tuple-oriented layout

Leaf nodes store separate packed key and value regions. Each has an offset
array for direct slicing without deserializing all tuples. Binary search reads
only keys; fetching a tuple obtains a contiguous slice of the value region.

Internal nodes store **upper-bound keys, child hashes and subtree entry counts**.
They never copy leaf tuple payloads. Full upper-bound keys keep routing and
range validation simple; prefix compression is a possible later optimization.

Canonical node encoding (all integers unsigned big-endian):

```
magic "DGPTR001"    8 bytes
level              uint32 (0 = leaf)
entry_count        uint32
key_region_bytes   uint32
value_region_bytes uint32
key_offsets        (entry_count + 1) uint32 values
value_offsets      (entry_count + 1) uint32 values
key_region         concatenated key bytes
value_region       concatenated values
```

An internal value is `child_hash[32] || subtree_entry_count[8]`. SHA-256 covers
the entire node encoding, including the format magic. Offset zero, monotonic
bounds, exact lengths, ordered keys, child count overflow, depth and child
address lengths are validated. Opening an imported root also validates its DAG.
The validator checks structural integrity, not whether a foreign producer used
this implementation's canonical chunking policy; only trusted producers should
be admitted into a canonical namespace without rebuilding.

The splitter examines a level-salted fingerprint of the last key after a
minimum payload size, with hard byte/entry limits. At least two entries are
required before a normal split so upper levels always shrink. Chunking is a
pure function of the ordered contents and fixed format parameters. Changing
those parameters requires a format/version decision.

Current constants: 512-byte minimum boundary eligibility, 8 KiB soft maximum
payload, 128 entries per node, 16 MiB per key/value, 32 levels. A large tuple can
exceed the soft chunk size; oversized values are not TOASTed by this prototype.
A chunk with two large items can be substantially larger than 8 KiB. Logical
entry counts count map entries, not keyless SQL row multiplicities.

The tuple helper is inspired by PostgreSQL's schema-driven tuple interpretation:
`DGTUP001`, attribute count, a present/not-null bitmap, an offset array, then
attribute bytes. NULL differs from an empty non-null attribute. It introduces
no runtime OIDs, XIDs, command IDs, lock bits, host padding or TOAST pointers.
It does not serialize PostgreSQL HeapTupleHeaderData. Types belong to an external
schema descriptor, analogous to TupleDesc. The helper supports at most 1,600
attributes and checks its total size against PL_MAX_ITEM.

Callers must supply canonical type encodings and order-preserving keys. The
included int64 key encoder is ordered, endian-independent and sign-correct.
This library does **not** claim SQL equality/ordering for raw numeric, text
collations, composite keys, user-defined types or raw heap bytes. Schema identity
must be incorporated by a higher-level table/database root. The earlier Python
SQL-specific schema/numeric/table encoder experiments were removed, not silently
advertised as implemented here. There is no keyless occurrence policy yet.

## Mutation, diff and merge

Edits stream the old ordered map and an ordered edit batch. Once a leaf boundary
resynchronizes, untouched leaf chunks are reused directly. Internal levels are
rebuilt from child descriptors and content-deduplicated. A fresh build and an
incremental edit of equivalent contents produce identical roots. The current
builder retains a leaf-node directory, not a materialization of all tuple bytes.
It still visits leaf references and reconstructs internal levels: this is not a
claim of O(log N) CPU per edit. Bounded-path parent mutation is future work.

Diff skips identical root/child hashes. Where internal separator ranges align,
it descends only into changed children. If boundaries or heights differ, it
reconciles the affected ordered streams and skips equal aligned leaf chunks.
This fallback is correct across split/coalesce transitions, though further
range-level skipping can improve it.

Three-way merge combines base-to-left and base-to-right diffs:

- Only one side changed a key: take that change.
- Both sides produced the same value or both deleted: accept the convergence.
- Different edits to the same key, including update/delete and unequal concurrent
  inserts: return PL_CONFLICT unless a resolver supplies a value/deletion.
- Fast-forward/equal-root cases simply reuse a root.

The merge uses memory proportional to differences, then applies the combined
edits through the normal incremental builder. It does not merge arbitrary SQL
schemas, validate relational constraints, or perform per-column conflict
resolution. A caller can implement a row-aware resolver with tuple_get when
schemas are compatible. Schema changes require an explicit compatibility rule
before attempting byte-level data merges.

## Tests and current limits

Nine native test groups cover:

- Tuple nulls, empty fields, binary values, offset validation and large payloads.
- Internal keys, lookup, range scans, counts and structural sharing.
- Independent branch edits, old-root persistence and convergent histories.
- 1,200 deterministic randomized edits compared with fresh builds in independent
  stores, plus deleting all keys back to the empty root.
- Diff through boundary/height changes, callback cancellation and cross-store roots.
- All 64 base/left/right combinations of absence, empty value and two distinct
  values; divergent inserts, update/delete conflicts and explicit resolution.
- Resolver scratch-buffer lifetime and unchanged output on conflict/error.
- Chunk export/import, digest corruption, digest-valid malformed offsets, missing
  children and complete reachable-DAG validation.
- A golden empty-root hash to detect accidental format drift.

The 5,000-entry localized-edit test creates three new chunks and reuses 119
unchanged leaf chunks with current parameters. These are regression observations,
not guarantees for arbitrary inputs.

Not implemented: on-disk chunk files, GC/ref persistence, WAL, SQL transactions,
PostgreSQL AM callbacks, collation/operator classes, schema/catalog versioning,
compression, prefix-truncated keys, parallel/thread-safe access, or out-of-line
large values. Store destruction releases all chunks; every retained root is
valid for the full store lifetime. No PostgreSQL core behavior is changed by
building this library.
