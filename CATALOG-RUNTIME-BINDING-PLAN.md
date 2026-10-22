# Versioned catalog runtime bindings

## Decision — 2026-10-22

This is the current next implementation plan for the
[branch-local DDL gate](CATALOG-DDL-PLAN.md), following the restricted CREATE
checkpoint. It records the agreed direction; the mapping described here is not
implemented yet. Preserve option 1: canonical schemas live beside canonical data
in prolly storage and native catalog providers resolve branch/revision context.

Separate logical names and object identity from PostgreSQL's local physical
relation names and OIDs. The current shared physical name is a bridge, not the
namespace authority. This separation removes the same-name collision and provides
the prerequisite for layout-changing ALTER without mutating another branch's
runtime descriptor.

## Identity and binding contract

Keep these concepts distinct:

| Concept | Responsibility |
| --- | --- |
| Logical namespace/name | Branch-visible SQL lookup, including search_path and revision-qualified references. The same name may denote different objects on different branches. |
| Logical object identity | Tracks an object across branches and schema changes; independent CREATEs must not be conflated just because their names match. |
| Canonical schema version | Retained portable definition and its dependencies, addressed by content. No local OIDs, transaction IDs or internal names in canonical bytes. |
| Runtime generation | Local descriptor/storage resources suitable for a particular schema and access pattern. Schema content equality alone does not prove storage bindings can be shared. |
| Per-reference binding | Resolved object/schema, branch or retained revision, snapshot/root selection, runtime relation where needed, and read/write authority. |

A local mapping resolves logical identity and schema generation to runtime
relations, indexes and necessary row-type identities. Branch context additionally
selects the correct data roots; a runtime OID alone must not select a branch.
Compatible branches may share resources when proven safe. Incompatible writable
definitions require separate runtime bindings with distinct internal names/OIDs.

Do not allocate one permanent OID or physical catalog copy per commit. Writable
generations acquire resources during DDL. Historical reads continue decoding
canonical retained descriptors directly, without user preparation or hidden
persistent query-time materialization. Any future transient runtime identities
needed by historical types must obey that read-only/standby requirement.

Before adding persistent fields, specify logical identity semantics for branch
copy, independent CREATE, DROP/recreate and eventual rename/merge. Decide which
identity metadata is canonical versus local. Do not introduce random allocation
history into canonical hashes and silently weaken history independence. Internal
name generation, mapping keys and catalog layout remain implementation choices;
write down those choices and their compatibility/migration consequences first.

## Ordered implementation gates

### A. Mapping and same-name CREATE — immediate milestone

1. Define the binding interface and local transactional mapping. Account for
   existing live physical relations without requiring a user migration command.
   Specify namespace/name conflict locks, mapping visibility and lifetime rules.
2. Resolve ordinary unqualified table names and write targets through canonical
   branch membership before opening runtime relations. Preserve ordinary PostgreSQL
   AM behavior. Qualification must select a context per reference, not switch a
   shared catalog view. Keep SQL aliases and error names logical.
3. Allocate distinct internal runtime names during branch-local CREATE. Table,
   index and row-type names must not collide. Check permissions on the logical
   namespace/object; internal storage naming must not grant extra access.
4. Carry the binding into planning/execution and INSERT, UPDATE, DELETE, COPY and
   index maintenance. Validate branch and schema generation before writes instead
   of deriving authority solely from the session branch or Relation pointer.
5. Make capture enumerate canonical members and resolve their bindings. Serialize
   logical definitions and names; never hash internal relation/index names or
   deparsed expressions containing them. Adapt DROP and checkout to the mapping.
   Checkout must not require one shared physical definition per logical name.
6. Publish physical resources, mapping, canonical schema, data/index roots and
   working membership atomically. Preserve explicit staging/HEAD behavior.

Gate A must pass before adding layout-changing ALTER. It must support the same
qualified SQL name with different built-in column definitions on two branches,
independent writes, and a single statement reading both versions. Include primary
and unique indexes so index ownership and internal naming are exercised.

### B. Successor generations and layout-changing ALTER

Implement a bounded ADD COLUMN / DROP COLUMN subset. Define the row transformation
and treatment of existing rows, nullability, indexes and dependent constraints
explicitly; retain guards for unsupported defaults, expressions and cascades.
Allocate a successor runtime generation rather than changing a descriptor used
by another branch or reader. Rebuild or transform data/indexes as necessary.
Publish the successor binding together with its schema and data roots.

Readers and portals retain their prior valid bindings. Conflicting same-branch
DDL/DML must block, rebind or fail with a documented retry outcome, never write
through a stale layout. Unrelated branches must not overwrite each other. Broad
serialization may remain for this proof, but its lock scope/order and limitations
must be explicit; this gate does not claim production concurrent publication.

### C. Logical catalog interfaces and dependency closure

Integrate supported pg_catalog/information_schema queries, regclass and relevant
OID/name conversions with the logical view. Define the context for direct catalog
queries and revision-qualified catalog access; do not assume a session switch
can explain two table versions in one statement. Prevent internal storage objects
from being presented as user tables or becoming an unintended access route.

Audit relcache, syscache, typcache, prepared-plan dependencies, whole-row values
and wire-visible row/type identities. Keep descriptors immutable for active
bindings. Distinct relation OIDs help existing relation caches but do not solve
all dependency-bearing lookups. Specify invalidation and retention for each
supported path rather than adding unscoped lookup exceptions.

Track dependencies against bound object generations. Extend to at least one
versioned custom type and one function with incompatible branch definitions,
including a default or constraint that depends on the function. Canonical
representations must use portable symbolic/versioned references, not raw OIDs
or raw PostgreSQL parse trees. Scope dependency checks and DROP behavior correctly.

Parts of C needed for correctness of A/B must be implemented there; C is not
permission to expose wrong catalog results or silently bind unsupported types.
Document and guard unimplemented interfaces. Full catalog viability is not
established until this broader gate passes.

### D. Review and expand coverage

Review the binding/caching boundary against the acceptance evidence before
expanding DDL forms, schema merge, concurrency or reclamation. Revisit the design
if correctness requires persistent query-time catalog preparation, shared mutable
schema views, or widespread context-free exceptions. Otherwise extend operation
coverage incrementally with the same invariants.

## Acceptance tests and reporting

| Gate | Required evidence |
| --- | --- |
| Same-name CREATE | Two branches create public.t with incompatible columns; each writes/reads its own definition; a cross-branch join returns both correctly. Same-branch duplicate CREATE and IF NOT EXISTS retain expected semantics. |
| Identity/storage | Identical supported logical definitions/data produce canonical identities independent of physical OID/name allocation; same-name objects do not share rows/index state accidentally. DROP/recreate has explicit identity behavior. |
| Transaction publication | Own DDL followed by DML; another session sees no uncommitted publication; full/savepoint abort; native index/transformation failure; restart and crash before/after commit expose complete states. |
| Readers/plans | Old snapshots, open cursors, prepared statements, checkout and DISCARD PLANS preserve/revalidate bindings. Incompatible prepared result changes fail explicitly. |
| ALTER | ADD/DROP COLUMN on one branch preserves the other and historical versions; transformed rows and indexes agree with the bound schema. |
| Authority | Ordinary owner/reader/unauthorized roles, search_path and qualified names; internal runtime names do not bypass logical permissions or branch membership. |
| Capture/retention | Add/commit/checkouts retain the right schemas and roots; no physical-name leakage; retained reads work cold, read-only and after restart, with standby coverage for affected read paths. |
| Dependencies/catalogs | Supported catalog views report logical names/definitions; incompatible whole-row/type/function contexts coexist in one statement; constraints resolve intended versions. |
| Compatibility | Inventory and run matching existing Doltgres revision/DDL tests, plus focused shared scenarios and native concurrency/recovery tests. Separate unsupported cases and intentional divergences from passes. |

For each checkpoint report exact selected suites, assertion counts, new/adapted
cases and failures. Do not weaken expected version semantics to fit the physical
bridge. Keep table-read success separate from proof of generalized catalogs.

## Publication, retention and remaining decisions

Use PostgreSQL's transaction/WAL decision for local mappings and runtime resources
alongside canonical objects and working refs; no second independently published
manifest. Canonical metadata remains portable and local mappings reconstructible.
Document the recovery/bootstrap path for a writable branch whose retained schema
has no local runtime generation, including branch creation/checkout from history.
It must not turn historical SELECT into a persistent materialization operation.

Initially retain generations instead of reclaiming them. This is a proof-stage
policy, not unbounded OID allocation per commit as a product architecture. Design
reader/portal/plan pins and resource reclamation before claiming long-term scale.
Mapping storage format, logical identity encoding, reuse policy, lock keys,
introspection context and type/function runtime identities require implementation
choices and tests; this document does not claim they are already settled.

Coordinate these details with [manifest publication](MANIFEST-PLAN.md) and
[row-level tracking](ROW-LEVEL-TRACKING-PLAN.md). General multi-branch writes,
schema merge, all PostgreSQL DDL forms and removal of table-wide writer locks
remain outside this bounded gate.
