# Catalog milestone 5: catalog lifetime and operational checks

## Decoded row reclamation

A CatalogView now separates its stable handle identity from decoded row storage.
The USERSET dolt_catalog_cache_entries target defaults to 16 resident production
views. At transaction completion, least-recently-used decoded rows and unreferenced
catcaches are reclaimed. Active scans pin row storage and may exceed the target.
Referenced cache tuples own their bytes and remain valid until released. The
canonical provider reloads cold rows from the immutable database object, reusing
the original handle map; no historical pg_class rows or extra files are written.

The target counts views, not bytes. Lightweight view/handle mappings, plus native
type and relation caches, still live for the backend lifetime: clients can retain
runtime OIDs without an explicit pin protocol. This bounds the expensive decoded
catalog residency, not all session memory. Persistent manifest/chunk garbage
collection is not implemented here. Long-running sessions with enormous numbers
of distinct schemas still need operational sizing and further cache work.

A provisional view records its creating transaction in backend memory. A handle
cannot make an aborted root visible, including after savepoint rollback. A root
created by the parent transaction remains usable after a child abort. If the same
canonical object is subsequently published again, normal visible-root binding can
reuse its identities. This transaction marker is not part of canonical bytes.

## Context and binding fixes found by tests

Generated array names must check types reconstructed in this load, not the old
handle map; otherwise reloading treats its own old array as a collision and tries
to allocate a different identity. Reconstructed arrays now keep their names and
handles even when table names collide with ordinary array names.

Function signature resolution follows a retained composite argument's context
when it differs from the live working view. This includes cached-plan reanalysis.
Live arguments preserve working-set execution authority. Mixed-context argument
lists retain normal resolution rather than arbitrarily choosing one context.
Explicit revision-qualified routine-name syntax remains separate work.

Branch binding preparation can reuse imported storage allocations across schema
metadata changes, just as independent allocations already could. Fallbacks check
relation kind. The imported physical bridge validates locator and AM, never the
canonical schema. A new branch still initializes from its retained descriptor,
not another branch's current data root.

## Parallel execution

Plans depending on revision-bound relations, catalogs, routines, composite types,
or runtime handle constants execute in the leader until worker context transport
exists. OID-producing expressions/parameters are conservatively unsafe: a cast can
reveal a runtime handle only after the initial hazard pass. Literal shared OIDs
remain safe. Worker-side guards reject contextual handle lookup or view creation
rather than silently consulting an unrelated physical catalog. Ordinary native
parallel execution is tested and retained. This is a deliberate performance
restriction, not worker context transport.

## Validation

The lifecycle TAP covers decoded row residency, repeated cold reloads, unchanged
runtime OIDs, collision-safe arrays, prepared routine/composite execution, held
catalog cursors, native and contextual parallel plans, read-only reads without
XID allocation, full/subtransaction abort, standby catalog/data reads and crash
recovery. Existing concurrent branch DDL/DML and snapshot tests run in the full
native suite. The Doltgres typed-wire test now runs with a one-view cache target;
new retained-signature assertions exercise reanalysis after ADD COLUMN. Original
native regression expected outputs remain unchanged.

The architectural gates are bounded. Remaining object-family codecs, general
multi-pass native DDL, cross-database shared dependencies, worker transport,
persistent garbage collection and row-level write concurrency are not declared
complete by this milestone.

Final verification: 39 native groups passed, including the 30-check lifecycle
TAP and unchanged PostgreSQL regression/isolation/PLpgSQL suites. The Doltgres
gate passed 1,033 script assertions across 42 test functions (also including
wire-protocol and harness checks). One earlier simultaneous native/compatibility
run timed out at nested DDL; the focused rerun and complete native rerun passed.
The test now sets a 15-second statement timeout after enabling forced parallel
execution. No underlying cause for that isolated timeout was established; retain
it as a stress-testing observation rather than claiming it was fixed.
