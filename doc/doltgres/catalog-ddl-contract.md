# Branch-local catalog DDL: audit and publication contract

Status: 2026-10-22, implementation-plan step 1. This is the audited interface
contract for the next implementation slice, not a claim that branch-local DDL is
implemented. The current guards remain. See [the gate](../../CATALOG-DDL-PLAN.md).

## 1. Evidence that changes the implementation boundary

An executable control in `test_prolly/t/012_catalog_ddl_contract.pl` starts a
REPEATABLE READ transaction without opening its target table, then adds a column
from another session. A SQL scan of pg_attribute in the original transaction
still sees the old column set, while the parser successfully binds the new column.
This is normal PostgreSQL behavior, not a PostgreSQL bug. It proves that feeding
ordinary catalog rows through SQL MVCC does not by itself give parser/relcache
lookups a branch/version context.

The source explains the result: `RelationIdGetRelation` caches by relation OID;
`SearchCatCacheInternal` keys on each cache's normal key columns. Its miss path
uses `systable_beginscan` with a NULL snapshot, which selects a catalog snapshot.
Neither cache key includes our branch or canonical schema generation. An ordinary
SQL catalog scan and a cache lookup therefore cannot be assumed to agree about
a historical definition. Holding AccessShareLock on a live table can block its
DDL, but does not make one physical identity represent incompatible branches.

## 2. DDL and identity cut points

| Operation | Existing entry points and effects | Required integration |
| --- | --- | --- |
| Dispatch | `tcop/utility.c`: `standard_ProcessUtility`, `ProcessUtilitySlow`, DDL guard before dispatch | Bind writable branch and operation scope before any shared catalog mutation. Preserve ordinary PostgreSQL dispatch for ordinary objects. |
| CREATE | `commands/tablecmds.c:DefineRelation`; `catalog/heap.c:heap_create_with_catalog`; row type, attributes, dependencies, indexes | Create symbolic definition plus branch-local storage binding together. A new table must not become visible on every branch. |
| ALTER | `AlterTableLookupRelation`, `ATController`, `ATPrepCmd`, `ATRewriteCatalogs`, `ATRewriteTables`; AM `relation_validate_alter` | Validate against the bound generation, stage successor schema and any row/index transformation, publish atomically. Layout-compatible changes can still alter constraints/dependencies. |
| DROP | `RemoveRelations`, `catalog/dependency.c:performMultipleDeletions`, storage unlink scheduling | Remove branch membership and scoped dependencies. Do not destroy storage required by another branch/history/reader. |
| DML | `ResultRelInfo`/`ModifyTable`, table/index AM callbacks take `Relation`; `dg_relation_branch` currently returns session branch | Carry explicit write binding to execution and storage. A relation pointer alone is not sufficient authority to choose a branch or schema. |
| Roots | `dg_store.c:load_state`/`dg_save`: relation-local records contain branch hash, XID/CID, content/handle roots | Couple a visible root to its decoding schema/generation outside canonical prolly bytes. Writes must validate this binding. |
| Capture | `dg_version.c:capture_inner`: scans live pg_class, locks shared catalogs, serializes definitions, exports selected branch roots | Enumerate the selected branch's bound membership/definitions. Current shared live enumeration cannot capture divergent branches. |
| Checkout | `dg_merge.inc:dolt_checkout`, `same_schema`, `lock_workspace` | Select branch without requiring schema equality or replacing another session's catalog. Retain schema-merge rejection separately. |
| Dependencies | `pg_proc.c:ProcedureCreate`, `pg_type.c:GenerateTypeDependencies`, `heap.c`, `dependency.c` | Preserve canonical symbolic references; scope runtime OID dependencies to object generations. Cascade must not cross branches by accident. |
| Planning/caches | `parse_relation.c`, `parse_type.c`, `parse_func.c`, relcache/catcache/typcache, plancache | Resolve all dependency-bearing objects through the binding context. Invalidation cannot mutate a descriptor still in use by another reference. |
| Introspection | SQL against pg_class/pg_attribute; information_schema views | Provide the selected branch's coherent logical catalog view. Fixing parser lookup alone cannot make these queries correct. |

The global activation guard in `dg_branch.c` is only one of the restrictions.
The table AM independently rejects layout-changing ALTER even on main before
activation. Tuple decoding currently requires the stored field count to equal
the descriptor's attribute count. Capture also rejects defaults, dropped columns,
user types and routines. Merely lifting the global guard would not implement even
ADD COLUMN correctly.

## 3. Interface decisions

The following is a semantic contract, not an exported C ABI or SQL API yet.
Implementation should introduce C interfaces as each consumer is migrated, rather
than add an unused interface with assumed lifetime semantics.

### Binding

A binding must contain database identity, explicit branch/ref selection, immutable
schema descriptor address, symbolic object identity, data/storage generation,
applicable SQL visibility context and read/write authority. An immutable historical
binding is read-only. A mutable branch binding is validated again for publication.
A table-reference context is independent of the session's default branch.

Resolve the logical name once against the appropriate snapshot plus own staged
changes. Return a pinned descriptor and dependency context. Type/function lookup
must not fall back to a current same-named object if the selected version is
missing or unsupported. A content hash is an equality key for a definition, not
an authorization token, lock key or complete object lifetime mechanism.

### Runtime identities and cache scope

Do not reuse one runtime OID for two simultaneously live incompatible definitions.
Use native descriptors where supported. Where PostgreSQL requires OIDs (for
example type/function execution), introduce a generation-specific runtime identity
bridge with explicit lookup, ownership, invalidation and retention rules. The
exact bridge representation remains an implementation choice to validate, not an
instruction to reconstruct physical tables per historical commit. Queries must
work cold without persistent metadata writes. Backend-local runtime caching, if
used, must be reconstructible and have no effect on availability.

Shared built-in OIDs remain unchanged. Runtime mapping IDs, branch incarnations,
XIDs, CIDs and publication tokens stay outside canonical schema/data hashes.
Canonical references remain symbolic/history-independent. Rename and name reuse
must not turn a deleted object's local runtime handle into a different object.

### Mutation

Separate `bind`, `prepare mutation`, `validate publication`, and `publish`.
DDL preparation receives the binding, analyzed command and SQL subtransaction.
It produces a staged schema/membership/dependency delta and the associated
storage/index changes. It does not mutate a shared live definition as the means
of selecting a branch. DML in the same transaction must bind the staged schema
with normal command ordering. Write callbacks must consume this explicit binding
rather than call `dg_current_branch()` as their identity source.

### Publication and reclamation

Retain heap/WAL-backed manifest infrastructure. Do not introduce an independent
filesystem manifest. Catalog/storage identity and visibility metadata belong to
the transactional runtime layer, not canonical prolly nodes. No reclamation is
part of this first slice: retain objects until refs, active readers, dependency
closures and in-flight transactions have a proven lifetime protocol.

## 4. Required publication state machine

1. **Bind:** under the SQL snapshot, select branch membership, schema and roots;
   overlay this transaction's earlier commands. Record the expected schema and
   publication generation. Do not mix a latest catalog definition with old rows.
2. **Prepare:** check ownership/dependencies and acquire locks for the affected
   branch/object. Build private successor descriptors, rows/index roots and
   dependency changes. A failed command rolls these back to its savepoint.
3. **Validate:** coordinate with competing DDL/DML; recheck the binding and actual
   modified-object set under the publication protocol. An unrelated branch change
   must not invalidate this operation merely because it shares a logical name.
   Concurrent incompatible schema changes fail explicitly or wait/rebind where
   PostgreSQL permits. Do not silently interpret old bytes with a new schema.
4. **Publish:** make the schema/data/dependency binding and corresponding manifest
   updates visible under the same PostgreSQL transaction outcome. Durable object
   payloads alone are not publication. A database-state ref must never resolve to
   half a DDL operation. Coordinate with row-level publication; do not claim that
   the present `working/*` ref already includes every ordinary DML change.
5. **Finish/abort:** commit invalidates affected future bindings but preserves
   pinned old readers. Abort/savepoint rollback removes own deltas and visibility;
   orphan immutable chunks may remain unreachable. Crash recovery must produce the
   same visibility decision for metadata, rows, indexes and refs.

Locks need branch/object scope plus schema-generation validation. A shared
physical relation lock is not sufficient branch isolation; the existing database
administration lock is not a production long-held DDL publication lock. Exact
lock representation/order and rebasing algorithms must be established alongside
row-level tracking. Three-way working-set merge remains useful, but schema merge
is not enabled by this contract and SQL consistency checks still apply.

## 5. Executed oracle and safety tests

- `TestInfoSchemaRevisionDb`: both original scripts pass on Doltgres (17 assertion
  statements). Both fail on the fork during setup at branch-local ADD COLUMN,
  before their assertions execute. The detached script later also requires USE,
  which is a separate unsupported interface.
- `TestCreateFunctionLanguagePlpgsql/Branching`: original script passes on Doltgres
  (11 assertion statements). The fork first fails at dolt_add because routine
  capture is unsupported. Later failures cascade from that missing setup; its
  32-character hash assertion is also engine-specific and will need the already
  established hash-length tolerance when this becomes a shared passing slice.
- New native `012_catalog_ddl_contract.pl`: 27 passing assertions. Covers the
  catalog snapshot/cache distinction, both DDL guards, aborted incompatible
  DROP/CREATE, retained history, savepoint failure preserving preceding DML,
  inter-session visibility, branch isolation and restart. Rejection tests verify
  safe boundaries; they do not count as implemented branch-local DDL.

Additional inventory: `TestCatalogHistory` supplies retained incompatible-schema
reads; `TestRevisionWorkingSets` supplies multi-branch data reads;
`TestCreateFunctionLanguagePlpgsql/"Create function on different branch"` is
skipped upstream; `TestSequences/"insert on a different branch"` is also skipped.
These skipped cases are design requirements, not a passing oracle. Custom-type,
concurrent DDL and publication-crash tests still need to be built for this gate.

## 6. Next implementation slice and unresolved decisions

Implement writable branch membership/schema bindings and transaction-local deltas
first; then route a bounded CREATE/DROP and ADD COLUMN subset through them,
including data writes and capture. Do not advertise an empty-table-only DDL demo
as viability. Preserve ordinary-user authorization and retention from the start.

The OID bridge for custom types/functions, wire-visible type metadata,
introspection provider strategy, branch-qualified function syntax and exact
lock/publication representation remain open. Do not select per-branch physical
schemas merely because they make the first DDL test easy: without the provider
and dependency contracts that would repeat the withdrawn projection approach.
The full gate remains open until these decisions have executable evidence.
