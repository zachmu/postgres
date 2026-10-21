# Retained catalog binding experiment

The hybrid remains the selected direction: canonical symbolic definitions plus
ordinary PostgreSQL runtime relation/type identities. The native experiment now
materializes actual retained manifest definitions and prolly contents, rather
than only the phase 1 fixture. It does **not** implement writable branch catalogs
or lift the existing checkout DDL freeze. See `catalog-decision.md` for the earlier
fixture and its distinct coverage.

## Interface and binding

```
SELECT dolt_catalog_materialize('HEAD~1');
SELECT dolt_catalog_materialize('HEAD');
SELECT old.v, current.v
FROM public.t AS OF 'HEAD~1' old
JOIN public.t AS OF 'HEAD' current USING (id);
```

Materialization is an explicit, superuser-only, standalone SELECT, allowed inside
a READ COMMITTED transaction. It creates immutable runtime namespace/table/row-type
and index objects. `pg_dolt_projection` maps (commit hash, logical namespace) to a
runtime namespace OID. The table definitions and rows come from the same retained
database root. Runtime OIDs, namespace names and PostgreSQL transaction metadata
never enter canonical history. Materialization creates a separate physical copy
per commit; this is deliberately a correctness experiment, not the final reuse or
storage-space strategy.

Reads perform no catalog writes. Prebuilt projections work in read-only
transactions and on a physical standby. `AS OF SYSTEM TIME 'revision'` is an alias
for literal revision selection, not timestamp resolution. Parameterized/time
expressions are not implemented. Unqualified AS OF references use existing
search_path namespaces, mapped to their logical names in the retained generation;
explicit schema qualification also works when that schema has since been dropped.

`"database@revision".schema.table` and `"database/revision".schema.table` select
retained commit data too; **named branches here denote their committed HEAD,
not their mutable working set**. Qualified row-type casts use the same mapping.
Use query aliases for column references. Unambiguous lowercase hexadecimal hash
prefixes of at least eight characters are accepted after named-ref lookup.
Qualified identifiers at the 63-byte limit are rejected because the lexer might
have truncated them. Use AS OF with a text literal for full 64-character hashes.
Cross-database access is not implemented.

A projected relation's AM binding is relation-specific and fixed to its own
runtime contents, independent of session checkout. Its ordinary relation OID is
sufficient to distinguish historical versions in the planner/executor. Normal
unqualified tables still use the existing session branch and branch-tagged roots.
This is not yet the production per-reference working-set binding service.

## Transactions and prepared plans

All projection catalogs, indexes, rows and root records use native PostgreSQL
transaction/WAL machinery. A same-generation object lock serializes competing
builders; a different generation does not share that lock. Rollback removes the
whole projection. A caller can prepare projection creation and a manifest ref CAS
in one transaction, recover from an immediate shutdown, and publish both with
COMMIT PREPARED. Ordinary dolt_commit/checkout/merge do not automatically invoke
this materializer yet.

Projected objects cannot be updated, truncated, renamed or dropped through the
tested relation/schema paths; dependency deletion also protects against schema
CASCADE. This is an internal superuser experiment, not a complete authorization
or immutable-object security boundary. Catalog generation GC is not implemented.

Projection-dependent analyzed queries carry a dependency on pg_dolt_ref. Ref CAS
sends normal relcache invalidation, but that alone is insufficient: a plan rebuilt
under an old REPEATABLE READ snapshot can otherwise escape into a later transaction.
The experiment therefore conservatively reanalyzes dependent cached queries on
every execution. The snapshot chooses the ref; full hash references still choose
the same immutable objects. Changed result types/collations retain PostgreSQL's
cached-plan error. This also conservatively affects plans directly reading the ref
catalog. Explicit ref/snapshot validity tokens are needed before optimizing reuse.

## Coverage and limits

The captured subset includes namespaces, tables with built-in column types and
collations, row types, NOT NULL, primary/unique constraints and secondary indexes.
Indexes are rebuilt through normal utility processing after loading retained
rows. Constraint indexes are not built twice. Runtime namespaces belong to the
materializing superuser; original owner, ACL and comment metadata are retained in
canonical objects but are not projected by this experiment. Do not expose these
interfaces to non-superusers as an authorization implementation.

Still unsupported: versioned functions, enum/domain definitions, views, defaults,
sequences, foreign keys, triggers, schema merges, mutable branch-specific DDL,
branch-aware pg_catalog/information_schema, portable stored expression binding,
generation GC, automatic publication-time preparation and schema-generation reuse.
A schema/data projection per commit is not a scalable production policy.

## Architectural decision

Retain the hybrid, with deeper **core binding and lifecycle integration**. Do not
attempt to fix these requirements with a session search_path or name hook alone.
The experiments support keeping existing relcache/syscache/typcache OID keys and
planner/executor operators; they do not prove complete catalog coverage.

Next implementation gates:

1. Carry database, logical object, branch/ref identity, schema generation,
   snapshot-visible data-root selection and write authority in a per-reference
   binding. Bind a prepared query to a named branch independently of later
   session checkout; full commit bindings remain immutable. Explicit HEAD has
   session-relative meaning in this experiment and needs a documented production
   policy. Ordinary unqualified prepared queries still have the old session-AM
   behavior; this slice does not fix them.
2. Separate schema-generation identities from data commits. Keep old generations
   alive across snapshots, cursors and prepared-plan dependencies; reuse compatible
   definitions without confusing data roots. Introduce no lazy catalog writes in
   read-only execution or standby replay.
3. Route CREATE/ALTER/DROP and dependency operations through a writable branch
   generation. Publish schema generation, data/index roots and refs atomically.
   Reconcile this with row-level publication work before removing the broad freeze.
4. Extend symbolic definition capture and binding to types, functions/operators,
   constraints and stored expressions, then implement branch-aware introspection
   and permissions. Namespace mapping is not sufficient for these paths.
5. Decide whether any irreducible OID/global-catalog assumption requires a deeper
   catalog lookup interface. No current test justifies rewriting all cache keys,
   but unsupported object families are still an explicit decision gate.

## Verification

`test_prolly/011_catalog_binding` covers real canonical materialization,
incompatible same-query schemas, row-type binding, index reconstruction, immutable
readers during source DDL, rollback, competing builders, prepared transactions,
crash recovery, read-only/standby reads, moving-ref plans and result descriptors.
The phase 1 `test_doltgres` tests continue covering mutable fixture DDL, enum
identities, privileges and lock independence. Those use heap fixtures, not native
writable versioned catalog publication.

Doltgres `TestAsOf` runs its original eleven query assertions unchanged, with
explicit preparation added only to external PostgreSQL setup. `TestCatalogHistory`
adds three assertions for incompatible definitions and a dropped table. Both run
against in-process Doltgres and the PostgreSQL fork. Function-on-another-branch,
sequence and historical cursor scripts were inspected; their unsupported function,
sequence, history-table and implicit-commit dependencies are not claimed as passing.
