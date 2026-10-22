# Transactional runtime mapping foundation

**Later checkpoint:** [same-name CREATE is now implemented](catalog-same-name.md).
The historical restrictions and test counts below describe this earlier milestone.

2026-10-22. First implementation checkpoint for
[the runtime binding plan](../../CATALOG-RUNTIME-BINDING-PLAN.md).
This implements the local mapping/publication foundation, not the full same-name
CREATE gate. Root planning documents are unchanged.

## Local representation and identity scope

The new heap-backed `pg_catalog.pg_dolt_binding` contains:

| Field | Meaning |
| --- | --- |
| bindbranch | Selected writable branch name. |
| bindname | Canonically quoted qualified logical member name, table or index. |
| bindschema | Address of its canonical schema object in retained storage. |
| bindrelid | Local physical runtime relation OID. |

The unique key is (branch, logical name). The schema address is a required version
check, not the logical object's identity. The OID identifies the currently usable
local resource. No random identity, internal name or OID is added to canonical
schema/data bytes. This does not yet assign stable portable object identities or
model rename/independent-CREATE lineage. It must not be interpreted as proving that
same-name or same-schema objects are interchangeable.

Mappings remain outside the hashed manifest graph. Canonical branch membership is
still authoritative for existence. Stale rows for removed members cannot confer
access. Mapping retention/GC is not implemented. Compatible branches currently
reuse the same physical relation and independently select branch-tagged data roots.

## Writers and visibility

Persistent capture records mappings for the exact table/index OIDs whose schemas
it serialized. Schema and mapping publication share the SQL transaction with
retained objects and refs. Mapping updates use CatalogTupleInsert/Update and CCI;
unchanged values do not rewrite a mapping. The writer scans under a registered
latest snapshot after acquiring a ShareRowExclusive lock on the binding catalog,
held until transaction end. This serializes mapping writers in this prototype;
ordinary readers retain MVCC visibility. It does not solve row-level concurrency.

Branch creation initializes mappings from already captured resources when there
is exactly one distinct candidate OID for the same logical name and schema address.
Checkout requires such a binding (and retains the existing physical-definition
validation). Ambiguity or a missing compatible generation fails closed. Branch
creation may retain historical commits without a writable runtime mapping; their
historical reads still work, but checkout remains restricted.

This reuse rule is deliberately a bridge while physical names remain unique. It
must be replaced/extended by explicit lineage and generation selection before
independent same-name resources are enabled; schema equality alone cannot decide
which writable generation to share.

## Read/write integration

Qualified working-branch queries now obtain their runtime relation from the
mapping and require its schema address to match the selected canonical table
descriptor. They no longer select that runtime relation merely by physical name.
The existing descriptor compatibility and permission checks remain.

Ordinary parser/AM membership checks additionally require the branch's mapping to
name the exact relation being opened/accessed and the expected canonical schema.
This rejects stale-generation ordinary reads, writes and AM paths. The existing
creation scope admits only newly allocated resources until their atomic capture.
Prepared prolly queries include the mapping catalog in their dependencies, alongside
the manifest refs; conservative snapshot-aware reanalysis remains in place.

Ordinary SQL name lookup itself still starts with physical catalogs. General
per-reference writable binding propagation is not implemented by these checks.
Root selection for writes still derives from the session branch. Thus this is a
validation foundation, not completion of the planned resolver/executor contract.

Historical AS OF queries remain independent of local runtime mappings. Working
reads never create or repair mappings, including read-only/standby execution.
Mapping repair/reuse belongs to explicit branch administration, not a prerequisite
users must run before otherwise supported ordinary queries. Missing mappings in
the fault-injection tests represent deliberately corrupted local state.

## Validation

- All 20 selected native groups pass, including test_prolly, test_doltgres and the
  231-test PostgreSQL core regression suite, with assertions enabled.
- The new 016_runtime_binding suite initially passed 30 assertions in that full
  run. Its expanded 32-assertion focused run adds prepared zero-row revalidation
  and a cold standby working read. Other coverage includes role isolation, schema
  and physical-generation mismatch, old snapshots, read-only operation, historical
  independence, atomic CREATE/savepoint rollback and crash recovery.
- Existing Doltgres compatibility tests pass unchanged: 131 assertions in ten
  focused suites (AsOf, CatalogHistory, NativeVersionedQueries,
  NativeHistoricalCursors, RevisionWorkingSets, DoltMergeData,
  PostgresMergeConflictStatus, ProllyVersionCompatibility, BranchCatalogDrop and
  BranchCatalogCreate, each prefixed Test). No new Doltgres conformance is claimed.
- The existing native mismatch test now expects the deliberate binding-unavailable
  diagnostic when a physical relation has been removed, instead of the previous
  name/shape-based error. Its rejection requirement is unchanged.

Fault injection uses superuser SQL against an isolated test catalog to corrupt or
remove local mappings. No public mapping mutation API was added; PUBLIC privileges
on the catalog are revoked. Native tests also assert historical reads remain valid
through such injected local failures.

The first integration run caught unsupported SQL ON CONFLICT on a system catalog,
an unregistered writer snapshot in an assert build, and omitted registration of
the new text-keyed unique index in IsCatalogTextUniqueIndexOid. The implementation
uses native catalog writes, registered snapshots and the catalog index registration;
all affected suites now pass.

## Deployment and next boundary

Adding a system catalog bumps CATALOG_VERSION_NO. This development binary requires
a newly initialized cluster (or future explicit catalog upgrade work); no in-place
cluster migration is provided and existing workspace databases were not modified.
Once the catalog exists, ordinary capture registers existing supported relations
without a separate user preparation function.

Same-name CREATE remains guarded. Next work must allocate distinct internal table,
index and row-type resources, resolve logical names before physical lookup, carry
explicit writable bindings through execution, and make capture serialize logical
names/definitions without physical deparser leakage. DROP and checkout must use
that authority too. Stable object lineage and writable bootstrap from retained
schemas without compatible resources still need implementation. ALTER remains
blocked on those prerequisites. Raw catalog introspection and generalized type/
function dependencies remain unresolved.
