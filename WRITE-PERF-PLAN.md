# Prolly write performance plan

Status, 2026-10-24: deferred. The chunk index is the next focus, and it replaces
step 1 below. Resume steps 2–4 once chunk storage and indexing are settled. The
evidence and reasoning here were gathered on the merge base at `d064539a616`.

## Motivation

Running the Doltgres-ported PostgreSQL regression suite against the fork stalled
during the prolly run's setup script. The suite's recordings come from
PostgreSQL 15.4, so its controls are other servers, not a perfect score:

| Server | Statements passed, of 42,090 |
|---|---|
| Stock PostgreSQL 18.6 | 97.63% |
| Fork, heap as default AM | 97.40% |
| Fork, `dolt_prolly_table` as default AM | Not scored: loading `tenk1` (10,000 rows) takes about an hour |

The fork's heap differences come from build options (no lz4, bundled timezone
data) and the extra Dolt access methods visible in catalogs. The replay harness
now supports external servers on doltgresql branch
`zachmu/regression-external-server`.

## Measurements

COPY of tenk1-shaped rows into an empty prolly table:

| Rows | Time | File size |
|---|---|---|
| 500 | 8 s | 27 MB |
| 1,000 | 33 s | 56 MB |
| 2,000 | 155 s | 117 MB |

Loading 500 more rows into a table that already holds 500 took 3.5 times as long
as the first 500. Insert cost grows with the table's history, not just the size
of the batch.

Parsing the relation file after a 1,000-row COPY:

| Measure | Value |
|---|---|
| Live trees reachable from the final root | 0.32 MB |
| All chunks ever written | 13.4 MB (42x the live trees) |
| File size | 58 MB (4.4x the chunk bytes; pages are 24% full) |
| Root records | 1,000, one full page each |
| Chunks split across two pages | 13% |

## Root causes

1. **Every load reads the whole file.** Each operation scans every page, rebuilds
   fragmented chunks, re-verifies every chunk's SHA-256, and imports everything
   into a temporary in-memory store. Inserts load once per row, including inside
   COPY.
2. **Every row commits a new root.** Each insert rewrites the leaf it touches and
   that leaf's parents, in both the content tree and the handle tree, and appends
   a root record. Content keys are digests, so each row lands on a random leaf.
   One 270-byte row writes about 14 KB of nodes.
3. **One record per page.** Each chunk fragment and each 168-byte root record
   gets its own 8 KB page, logged as a full-page WAL image.
4. **Validation walks the whole tree on every load.** `pl_validate` visits every
   node of both trees each time.
5. **Smaller costs that matter once loading is fixed:**
   - The catalog binding check runs several SPI queries on every load.
   - `pl_apply` walks every leaf instead of only the edited path.
   - Indexed inserts rebuild each whole index.
   - Index scans match index rows to handles with a nested loop.

## Plan

Each step goes on its own branch, which is merged into an integration branch
`zachmu/write-perf`. Measure after every step.

### Step 1 — Chunk cache (superseded by the chunk index)

The original step was a per-session cache. It would mirror each relation file in
memory, read only blocks appended since the last load, remember root records, and
evict whole relations when over budget. The redirection was:

- The cache must be shared across sessions. Chunks are immutable, so sharing is
  safe.
- Locating a chunk by hash must not require scanning storage, at startup or on
  first access. That calls for an index of chunk locations serialized to disk,
  which becomes a separate effort.

Pieces from the discarded draft that still apply to whatever replaces it:

- **Per-node validation memo.** Nodes are immutable, so a node whose subtree has
  passed validation can be marked validated. Revalidating such a subtree under a
  new parent then only checks the leftmost leaf's first key against the parent's
  lower bound. The draft included a library test proving that a memoized subtree
  still fails under a bad lower bound.
- **Writes check the target file.** Save walks from the new roots and writes
  only nodes the *target* relation file lacks. It writes children before parents,
  so a node present in a file implies its whole subtree is present, even after a
  failed append. A state can borrow another relation's store, as native catalog
  rewrites do, so the "already written" set must belong to the target relation.
- **Truncation must invalidate cached state.** A nontransactional truncate
  shrinks files in place. Both table and index files reach `RelationTruncate`
  directly, so a hook there covers every case.

### Step 2 — Packed appends

- Append many records to the relation's last page until it is full, instead of
  one record per page. Log appends to an existing page as generic WAL deltas,
  not full-page images.
- Let a large chunk's first fragment fill the remaining space on the last page,
  instead of capping chunk size, which would change tree shapes and hashes.
- Shrink the record header where possible. Total length and offset only matter
  for fragmented chunks.
- Constraint: never store physical locations (block or item numbers) inside
  nodes or roots. A future VACUUM would copy live chunks forward into a new file,
  and doesn't care whether records are packed.

Expected effect: page fill rises from about 24% to nearly 100%, and WAL volume
falls to roughly the bytes written.

### Step 3 — Statement-level deferred flush

Root records already carry the writing statement's command ID. Root versions
from one statement share that ID, so snapshot visibility can't tell them apart.
Only the statement's own self-snapshot reads see state from inside the statement.
Writing only the last root of each statement, plus the nodes it reaches,
therefore preserves today's semantics.

- **Pending state.** Keep new nodes and a pending root per relation and branch
  in backend memory. Same-statement self-snapshot reads use the pending root.
- **Flush triggers.**
  - Lazily, in the load path, when the command ID has advanced.
  - When a subtransaction starts.
  - Before commit and before PREPARE.
- **Abort.** Transaction or subtransaction abort discards pending state.
- **The flush.** Walk down from the pending root, stopping at nodes already
  written. Append the new nodes, then the root record last.
- **Memory bound.**
  - Free superseded pending nodes once no loaded state pins them.
  - If pending bytes cross a threshold mid-statement, write the reachable new
    chunks without a root. A chunk with no root is inert.

Coordinate with [row-level tracking](ROW-LEVEL-TRACKING-PLAN.md). Its model keeps
transaction changes private until publication, which is compatible. Design for
concurrent writers to one table and branch. Don't depend on the current
relation-wide append lock, which is temporary.

Expected effect: COPY and INSERT…SELECT write about one tree version per
statement instead of one per row. Single-row autocommit statements still write
one version each.

### Step 4 — Batched multi-row inserts

`multi_insert` currently calls the single-row insert in a loop. Instead, load
once, encode all rows, and apply one sorted batch of edits to each tree. That
means multiplicity edits for the content tree and sequential handle edits for
the handle tree. Then save once per batch. This reduces CPU work; step 3 already
removes most of the write amplification.

## Benchmarks required on completion

Report inserts of varied sizes before and after the changes, with graphs of time
and disk growth against insert size. Measure WAL bytes too. Cover:

- COPY into an empty table and into a populated table.
- A single INSERT…SELECT.
- Batches of single-row autocommit INSERTs.
- A table with and without a secondary index.

Keep baseline runs small enough to finish on the merge base. On the baseline,
COPY of 2,000 rows already takes 155 s and costs grow quadratically. Run large
batches only on the improved build.

## Explicitly out of scope here

- Incremental index maintenance and logarithmic index scans. Today each indexed
  insert rebuilds the whole index, and scans use a nested loop.
- Memoizing the per-load catalog binding check.
- Making `pl_apply` touch only the edited path.
- VACUUM, garbage collection and freezing.
- Compression, and encoding padded `name`/`char(n)` values compactly.

## Related findings

The `pg_dolt*` catalog audit (`reports/pg-dolt-catalogs.html` in the workspace)
found redundancies that affect storage design:

- Committed chunks are copied into `pg_dolt_object` at every capture, which walks
  every tree.
- Live data and history use two hash namespaces.
- A branch's working state is recorded in three places. Branch rename loses
  uncommitted work as a result.

These will be resolved together with chunk storage and indexing.
