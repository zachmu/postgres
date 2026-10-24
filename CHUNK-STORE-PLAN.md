# Chunk store plan

Status, 2026-10-25: milestone 1 is complete (commit `645794b3460` on branch
`zachmu/chunk-index`): a minimal catalog with native access, a node cache and
microbenchmarks. The heap design is accepted provisionally. We build on it only
behind a standard store interface (below), so a different implementation can
replace it later. Milestone 2, the interface, is complete (`b8885f6124d`).
Milestones 3 and 4, library changes and AM integration, are next. This plan supersedes step 1 of
[the write performance plan](WRITE-PERF-PLAN.md) and is the next focus of
[PLAN.md](PLAN.md).

## Problem

Two partial chunk stores exist today. Neither meets the requirements.

- **Prolly relation files.** Each table and each prolly index has an append-only
  file of nodes and root records, protected by WAL. There's no index by hash:
  every load scans the whole file. Each file is also scoped to one relation, so
  it can't answer "where is chunk X" for a database.
- **`pg_dolt_object`.** A heap catalog with a btree on `objhash`, so it's
  database-wide and persistent. But it's keyed by an envelope hash, not the prolly
  node hash. It holds only chunks exported at commit or capture, which duplicates
  relation data. It allows duplicate rows, writes through SPI, and TOASTs large
  payloads.

The pg_dolt catalog audit (workspace `reports/pg-dolt-catalogs.html`) lists
the consequences: copying on every capture, two hash
namespaces, and a full tree walk per capture.

## Requirements

1. Any chunk is locatable by hash quickly, across every table, branch and commit.
   Commands such as merge take arbitrary commit hashes.
2. The index is serialized to disk. No relation is scanned at startup or first
   access.
3. Cached chunks are shared across sessions. Chunks are immutable, so sharing is
   safe.
4. Row-level concurrent writers to one table on one branch must be supported
   ([row-level tracking](ROW-LEVEL-TRACKING-PLAN.md)). The current table-wide
   writer lock is temporary. There must be no hot page or relation-wide append
   lock on the write path.
5. One hash namespace: chunks are keyed by the prolly node hash, so tree
   pointers are store keys directly.
6. A chunk hash is unique by definition. Duplicates are bugs.

## Division of responsibility

**Table and index access methods stay as adapters into PostgreSQL's executor.**
Their own relation storage holds only non-canonical runtime state:
- which root is current per branch and snapshot (root records today);
- the mapping between TIDs and rows (handle trees today; later the row identity,
  version and lock state of the row-level plan);
- any other per-transaction metadata.

This state is updated outside prolly content and has to live in a relation that
works with PostgreSQL's APIs. The AMs also own the tuple codec.

**The chunk store is database-wide and holds every canonical node:** table
content trees, index trees and schema trees, whether working or committed. A
commit records root hashes and copies nothing.

**Structured metadata objects stay separate:** commits, database states, table
and index descriptors, tags, merge, rebase, stash and routine objects. They stay
in `pg_dolt_object`, or a successor, rekeyed onto the same node-hash namespace
and with duplicates fixed.

## Selected design: heap catalog with a unique btree

```
pg_dolt_chunk (OID 9125)
    chunkhash  bytea  -- SHA-256 of the node bytes; primary key
    chunkdata  bytea  -- node bytes, inline, uncompressed
pg_dolt_chunk_hash_index (OID 9126): UNIQUE btree (chunkhash)
```

### Decisions

1. **A separate catalog for chunks**, distinct from metadata objects.
2. **Insert semantics:** transactional insert-if-absent versus immediately
   visible chunks is undecided, pending measurement. Milestone 1 uses
   transactional insertion following PostgreSQL's ON CONFLICT DO NOTHING
   protocol. That means:
   - a dirty-snapshot probe;
   - waiting for an in-progress inserter of the same hash;
   - a speculative heap insert;
   - a unique-index check that cancels and retries on conflict.

   Aborted inserts become dead tuples that VACUUM reclaims, with no chunk
   garbage collection needed. The alternative makes chunks visible immediately,
   avoiding waits between writers that create identical chunks. It moves cleanup
   of unreferenced chunks onto garbage collection.
3. **All data inline, no TOAST.** The catalog has no TOAST table and its data
   column uses plain storage, with no compression. A chunk must fit in one heap
   tuple, about 8,100 bytes of node data.
   - Today's chunker can exceed that. Nodes reach about 8,500 bytes, and single
     large values make larger ones.
   - Before AM integration, the prolly library must bound node size below the
     inline limit. Large values need their own chunked representation, as Dolt
     does for blobs.
   - Milestone 1 rejects chunks over the limit.
4. **Direct access in all cases:** heap and index C APIs, never SPI.
   - Reads probe the unique index with a non-MVCC snapshot, like TOAST. Chunk
     content is fixed by its hash, and any root a reader can see references
     only chunks that exist.
   - Writers probe with a dirty snapshot to dedupe.
5. **A per-backend node cache is required.** It's keyed by hash and bounded by
   `dolt_chunk_cache_size`. It never needs invalidation, because nodes are
   immutable. Every performance comparison includes it.
6. **One hash namespace:** the prolly node hash.

### Locking and maintenance

- Readers take ACCESS SHARE on the catalog and index, and writers take ROW
  EXCLUSIVE, so scans never block writes.
- Page-level content locks are short and never held while waiting on a
  heavyweight lock.
- Garbage collection deletes unreachable rows, and plain VACUUM reclaims space.
  Routine maintenance never takes ACCESS EXCLUSIVE on the chunk store, so no
  VACUUM FULL or CLUSTER.

### Transactions

The chunk store, the AM runtime stores and branch pointers can all change in one
PostgreSQL transaction. They share one commit record, one WAL stream and one
recovery path. Requirements that remain:

- **A garbage collection protocol.** A writer may skip inserting a chunk it sees
  and reference it, while garbage collection decides that chunk is unreachable.
  MVCC alone doesn't prevent deleting it. Dolt's garbage collection solves the
  same race with a safepoint protocol.
- **Branch-pointer publication** must not hold one hot row across transactions.
  It merges onto the latest root at commit
  ([row-level tracking §4](ROW-LEVEL-TRACKING-PLAN.md)).
- **No buffer or lightweight lock held while waiting on heavyweight locks.**
  PostgreSQL's deadlock detector doesn't see those.
- **Subtransaction abort** discards backend-local caches and pending state that
  belong to the aborted subtransaction.
- **Logical decoding** won't see prolly data written with generic WAL.

### Reading at access time

Table and index AMs fetch nodes from the chunk store while scanning, as heap
tuples read TOAST. Costs and mitigations:

- **Index probe plus heap fetch per uncached node**, a few microseconds. The node
  cache absorbs hot interior nodes.
- **Inline, uncompressed storage** lets a node be parsed from a pinned buffer.
- **Scattered leaves on a cold cache.** Mitigated with read-ahead from interior
  nodes through read streams.

### Measured limits (milestone 1)

Full results are in the workspace report `reports/chunk-store-benchmarks.html`.
Setup: an optimized build, 7 CPUs, 7.6 GB RAM, 2 GB shared_buffers.

- **Space.** Overhead is about 75 bytes per chunk. Mixed sizes from 512 to 8,000
  bytes pack into heap pages at about 90% efficiency.
- **Writes are bound by WAL volume.** Each chunk writes its own size to WAL plus
  about 5%.
  - One session does about 46,000 mixed-size puts per second at first, slowing
    to about 30,000 by 2 million chunks (9.5 GB) as heap write-back competes.
  - Four sessions top out near 45,000 per second, at about 215 MB/s of WAL.
- **The write curve bends at shared_buffers, not RAM.** With 128-byte chunks,
  puts run at 130,000 to 200,000 per second until the btree passes
  shared_buffers, at about 27 million chunks. They then fall steadily, to
  22,000 per second at 68 million chunks and 4.9 GB of index. Random-key inserts
  read and dirty random leaf pages. This curve is accepted for now.
- **Reads.**
  - A warm native lookup takes 1.5–3.4 µs, about 25% faster than SPI for mixed
    sizes.
  - A node-cache hit takes 0.06–0.1 µs.
  - Reads that miss memory cost 30–150 µs on either path.
- **Hint bits.** Data checksums are on by default in PostgreSQL 18. With them,
  the first existence probe of newly committed chunks sets hint bits and logs
  full-page images: about 850 MB of WAL per 200,000 probes. Plain reads skip
  visibility checks and avoid it.
  - Fix before AM integration: probe without setting hints, or freeze chunks
    promptly.
- **Write-path consequence for AM integration.** Without the statement-level
  deferred flush, each inserted row writes its leaf and the path above it, about
  14 KB of chunks. At about 200 MB/s of WAL, that caps inserts near 15,000 rows
  per second. The deferred flush
  ([write performance plan](WRITE-PERF-PLAN.md) step 3) therefore follows AM
  integration immediately.

## Store interface

Every consumer goes through one interface. Implementations sit behind a function
table, in the style of PostgreSQL's table AM routine. The heap catalog is the
first implementation. A packed journal, or anything else, must be able to
replace it without changing callers.

### Contract callers may rely on

- **Idempotent puts.** Putting a chunk that is already present is harmless.
- **Reachable chunks are readable.** A chunk referenced by any root visible to
  the caller's snapshot can be read.
- **Durability by commit.** A chunk put in a transaction is durable once that
  transaction commits.
- **Content is fixed by hash.** The bytes returned for a hash are the bytes that
  were put under it.

### Not part of the contract

Callers must not rely on these, even though the heap implementation shows them:

- whether a put disappears on rollback;
- whether a put waits for a concurrent inserter of the same hash;
- whether chunks put by other running transactions are visible;
- relation lock modes, catalog OIDs or snapshot types;
- how long results remain valid without an explicit pin.

### Interface shape

- **Open and close by intent,** read or write, not by lock mode. A handle and
  its pins last at most one transaction.
- **Batch get.** Returns each found chunk *pinned*. Callers release pins
  explicitly, and pins are dropped at transaction end. A single-chunk call is a
  convenience wrapper.
- **Batch existence check.**
- **Batch put.** The implementation may reorder a batch, for example by hash for
  index locality.
- **Lifecycle operations:**
  - enumerate and delete, for garbage collection;
  - the maximum chunk size, reported by the implementation;
  - store statistics.

### Above the interface

These layers wrap every implementation, so benchmarks compare stores rather
than caches:

- **The node cache.** Pinned entries are never evicted, and eviction uses only
  unpinned entries.
- **Optional hash verification** of puts and cache fills.
- **Statistics.**

Implementation selection is a single routine pointer. Tests can install another
implementation to prove the boundary holds.

## Alternative: custom packed journal with a serialized index

Kept in reserve in case write throughput or index scale outgrows the btree.
Modeled on Dolt's NBS journal and table files:

- **Chunk records** appended to relation pages, many records per page, with
  generic WAL deltas instead of full-page images.
- **Sorted runs** indexing those chunks, about 14 bytes per entry (an 8-byte hash
  prefix, block and item). Each run has a page directory and a Bloom filter, and
  is searched in place through shared_buffers.
- **Size-tiered merging.** Small merges happen inline at publication; large ones
  go to a background worker that appends output in small batches.
- **The list of live runs** is a heap catalog of run descriptors, inserted
  transactionally at publication. An earlier variant with a metapage in block 0
  was rejected: under row-level concurrency it becomes a hot page.
- **Database-wide index entries** carry a storage id, block and item.

| | Heap and btree (selected) | Packed journal |
|---|---|---|
| Write path | Random btree leaf per chunk, plus WAL full-page images after checkpoints | Sequential appends; index writes amortized by merging |
| Index size per chunk | ~50 B | ~14 B, plus ~1.2 B of Bloom filter |
| Lookup | One btree descent | A Bloom filter per run, then a binary search in one or two runs |
| Garbage collection and space | DELETE and VACUUM, space reused in place | Copy-forward compaction; runs rewritten |
| Concurrency, recovery, replication | Provided by PostgreSQL | Custom protocols to write and test |
| Implementation effort | Small | Large |

**Revisit when** measurements show the btree index outgrowing memory for real
workloads, or index maintenance dominating write throughput.

**Intermediate option before a full journal:** a small recent index plus a large
settled one, merged periodically. That approximates sorted runs using
PostgreSQL's own structures.

## Milestones

1. **Minimal store (done, `645794b3460`):**
   - the catalog, insert-if-absent, lookup and existence check;
   - the node cache and `dolt_chunk_cache_size`;
   - correctness tests: dedup, abort, concurrent identical inserts, recovery, the
     size limit;
   - microbenchmarks of puts and gets with and without the cache at several
     scales, recording WAL bytes per chunk.
2. **Store interface (done, `b8885f6124d`):**
   - the function table, contract and batch and pin API above;
   - the cache, verification and statistics moved above it;
   - the heap implementation behind it;
   - a second, test-only implementation exercising the same contract tests;
   - an existence probe that doesn't set hint bits.

   As built:
   - `access/doltchunk.h` defines `DoltChunkStoreRoutine`, implemented in
     `dg_chunk_heap.c`.
   - `dg_chunk.c` holds the cache, pins, verification and dispatch.
   - `dolt_chunk_verify_hashes` controls verification.
   - `test_dolt_chunk` runs one contract file against the heap store and a
     non-transactional in-memory store. It uses pageinspect to prove probes
     leave hint bits unset.
3. **Library changes:**
   - bound node size below the inline limit, and represent large values in
     chunks;
   - give the prolly library a fetch-on-miss callback, so trees read through the
     store and node cache.
4. **AM integration.**
   - Table and index content trees are written to and read from the chunk store.
   - Relation files keep only roots and runtime state.
   - Decide where root pointers and handle maps live.
5. **Deferred flush:**
   [write performance plan](WRITE-PERF-PLAN.md) step 3, required before the
   integrated write path is usable at volume.
6. **History.**
   - Commit and capture record roots without copying.
   - Rekey and dedupe `pg_dolt_object`.
   - Resolve the working-state duplication found in the audit, including branch
     rename losing uncommitted work.
7. **Garbage collection.** Define the reachability and safepoint protocol, then
   delete and VACUUM.

After milestone 5, re-evaluate
[the write performance plan](WRITE-PERF-PLAN.md) steps 2 and 4 against the new
storage. Packed appends may no longer apply to chunks.
