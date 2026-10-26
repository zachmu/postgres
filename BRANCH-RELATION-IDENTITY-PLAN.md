# Branch relation identity and resolution

Decided 2026-10-25. This plan supersedes the runtime-handle parts of
[the runtime binding plan](CATALOG-RUNTIME-BINDING-PLAN.md#current-runtime-contract--2026-10-22)
and the read path of [the catalog binding plan](CATALOG-BINDING-PLAN.md). The
canonical schema provider, per-reference context and storage resources from
those plans remain.

## Requirements

- **Every database is multi-branch.** There is never a period in which sessions
  cannot sit on different branches. Every reference to a prolly table resolves
  through a ref's canonical schema, from the first statement onward.
- **Sessions select a branch; names select a ref.** Checkout changes which
  branch a session's unqualified names resolve to. Revision-qualified names
  resolve the same way in every session. See
  [Dolt branches](https://www.dolthub.com/docs/sql-reference/version-control/branches/).
- **Ordinary relations.** A resolved table is an ordinary relation to the
  planner and executor: heap-style scans, index paths, estimates, statistics,
  system columns, row locks, triggers. Which prolly root holds its data is an
  implementation detail of the table and index AMs.
- **Branch isolation for locks.** DDL on one branch never blocks a session using
  another branch.
- **WAL and hot standby.** Locks that PostgreSQL logs for standbys, and the
  invalidations it replays, must mean the same thing on primary and standby.
  Standbys run this fork, so the standby side may change.
- **History independence.** OIDs never enter prolly storage or any hash. They may
  be stored durably outside prolly.
- **Bounded identity.** No OID per revision. Revisions are unbounded; branches
  are not.

## Current state and what is wrong with it

Read on branch `zachmu/doltgres-rewrite` at `677eb42109c`.

| Behavior | Code | Problem |
| --- | --- | --- |
| A `views/enabled` ref, set by the first `dolt_checkout` or first ALTER of a prolly table, switches the whole database to versioned resolution, permanently. Before it, prolly tables resolve through physical catalogs. | `dg_catalog_active`, `dolt_checkout`, `alter_activate` | Two resolution stacks for one table; plans change for every session at an arbitrary moment; physical rows supply versioned definitions. |
| Reads of a resolved table are a custom range-table kind planned as DoltVersionedScan, which walks the content tree itself. | `addRangeTableEntryForDolt`, `dg_catalog_path`, `dg_catalog_scan.c` | No index paths, fixed 1,000-row estimate, no TIDs, no `ctid`, no row locks, so `SELECT FOR UPDATE` is rejected and RI checks fall back to a branch-wide lock. |
| Write targets bind to an ordinary relation entry over a definition handle; the executor opens a private storage relation for the branch at run time and attaches indexes privately. | `dg_catalog_target`, `dg_catalog_executor_target` | Indexes are invisible to the planner. |
| The planner ignores the indexes of every catalog-view relation. | `get_relation_info` in `plancat.c` | Primary-key lookups are full scans. |
| Definition handles are OIDs from a counter in each backend, bound to an in-memory catalog view per working-schema hash. | `CatalogViewObject` in `catalogview.c` | Different OIDs for one table in different backends; any DDL on a branch renumbers every object on it; unusable for locks, invalidation, statistics, parallel workers or WAL. |
| The AM reads the session's branch for native relations. | `dg_relation_branch` | A cross-branch query reads the wrong roots. |

The context-bound lookup machinery works as designed: a relcache build for a
handle switches into the handle's catalog view and reads its own catcaches, and
name lookups use the session's view. This plan keeps that machinery and changes
what its keys are.

## Design

### Identity

| Reference | Identity | Lifetime | Locks |
| --- | --- | --- | --- |
| A table or index on a named branch head | Durable OID per (branch, logical object) | Created with the branch or object; deleted with either | All modes, logged and replayed as today |
| A table or index in a revision (commit, tag, `AS OF`) | Leased OID for (schema object hash, logical object) | While referenced | AccessShare only |

**Durable branch OIDs.** A new catalog, or new columns on `pg_dolt_binding`,
maps (branch, logical object identity) to an OID allocated with
`GetNewOidWithIndex`. Rows are created for every table and index when a branch is
created (O(tables), never O(rows)) and when an object is created on a branch.
They are deleted when the object is dropped or the branch deleted, and vacuum
reclaims them. A branch rename is a delete and re-create. Row types get durable
OIDs the same way. The mapping is local metadata outside prolly storage; canonical
schemas keep portable references.

**Leased revision OIDs.** A shared-memory registry assigns OIDs from the
reserved range to (schema object hash, logical object) on first use, and reclaims
an entry once no plan, portal, relcache entry or transaction references it. These
relations are read-only and immutable, so they need no invalidation and never
reach WAL. A standby keeps its own registry.

### Name resolution

Every reference to a prolly object resolves through the canonical schema of a
ref: the session's branch for unqualified names, the named ref otherwise. The
result is an ordinary relation range-table entry carrying a durable OID (branch
head) or a leased OID (revision). The `views/enabled` ref and the physical
fallback are removed. `regclass` input and other name-to-OID conversions follow
the same rule.

### Catalog rows

Relcache, syscache and type-cache lookups by a prolly OID map it to (ref,
logical object) and build catalog rows from that ref's canonical schema, using
the existing catalog-view machinery re-keyed by OID:

- **Branch heads** read the branch's working schema under the catalog snapshot,
  the "latest committed" rule PostgreSQL uses for physical catalogs. DDL commits
  a new working schema and queues ordinary invalidation messages for the branch's
  OIDs, which replay correctly on standbys because the OIDs are durable. Other
  branches' OIDs are untouched.
- **Revisions** read the immutable schema object named by the lease.
- **Catalog queries** (`pg_class`, `information_schema`) show the session's
  branch, and revision-qualified catalog references show that ref, as today.

Physical per-branch copies of `pg_class` and related catalogs were considered and
rejected: they duplicate the canonical schema, collide on catalog unique indexes
and re-create the persistent projection earlier plans ruled out.

### Locks, WAL and standbys

Heavyweight relation locks use the durable OID, so they are branch-scoped by
construction. AccessExclusive locks are logged with an OID the standby already
has from the replicated mapping. A standby query resolves the same durable OID
and takes AccessShare on it, so replay conflicts exactly as for heap. Revision
locks are AccessShare only, which PostgreSQL never logs. The mapping is created
eagerly so that no standby reader can hold a lock on an identity the primary has
not yet allocated.

Row locks keep using `pg_dolt_lock` records keyed by storage and branch.

### Data access

The table and index AMs take the ref from the relation: a branch name for
branch heads, a root hash for revisions. They never consult the session's branch.
Storage resources keep supplying file locators for buffers and WAL.

### Planning

Built catalog rows include real index OIDs, so the planner gets index paths,
unique proofs and estimates through unchanged code. Remove the catalog-view
exception in `get_relation_info`. Store statistics per durable OID.

### Removals

DoltVersionedScan and its custom range-table kind, the `views/enabled` marker,
backend-local definition handles for branch heads, the executor's separate
write-target binding, and the RI fallbacks that skip `FOR KEY SHARE` and take a
branch-wide lock.

## Milestones

1. **Mapping.** Durable OIDs for tables, indexes and row types; creation at
   branch and object creation; deletion at drop; rename as delete and re-create.
   Tests: allocation, replication to a standby, deletion and reuse.
2. **Resolution and catalog rows.** Resolve every reference through a ref,
   remove the marker, key catalog views by durable OID with catalog-snapshot
   reads and invalidation. Tests: same-name incompatible tables on two branches
   read in one query; DDL on one branch invisible to another branch's open
   sessions, cursors and prepared plans; standby sees new schemas after replay.
3. **Ordinary access.** The AM takes the ref from the relation; reads become
   ordinary scans; index paths and estimates; remove DoltVersionedScan and the
   write-target path. Tests: primary-key lookups use index scans on every
   branch; `ctid`, `SELECT FOR UPDATE` and RI `FOR KEY SHARE` after branching;
   a cross-branch join.
4. **Revisions.** Leased OIDs for revision references, read-only enforcement,
   reclamation. Tests: `AS OF` and commit-qualified reads, many revisions in a
   long session without OID growth.
5. **Locks and standby.** DDL on one branch never blocks another; standby lock
   replay conflicts on the same branch only. Tests: two-branch DDL concurrency
   on a primary and on a hot standby.
6. **Catalog consumer coverage.** Inventory every catalog consumer (`pg_class`
   and `information_schema` scans, `regclass`, `pg_depend`, types, functions,
   statistics) and test each against two branches.

Acceptance: the isolation suite passes the same tests whether or not the database
has more than one branch; core regression, the prolly and doltgres suites and the
Doltgres revision tests pass.

## Open questions

- Logical object identity across branch copy, independent same-name CREATE,
  DROP and re-create: what the mapping is keyed by, beyond the name.
- How statistics and `pg_statistic` rows are created for a new branch: copied
  from the source branch or rebuilt.
- Registry reuse safety for leased OIDs: an epoch or invalidation broadcast so no
  backend holds a cached entry for a reused OID.
- Logical replication of prolly tables, which needs a decoder for root changes
  and publications keyed by logical identity. Out of scope here.

## Status — 2026-10-26

Milestones 1 to 5 are implemented and merged; milestone 6 is partial. The
isolation suite gives byte-identical results with and without an extra
branch (39 of 121 specs pass with prolly as the default AM, against 58 before
the marker was removed). Core regression passes on heap.

Decisions made during implementation:

- Branch-head schemas, storage bindings and names resolve under the latest
  committed catalog state, as PostgreSQL catalogs do; SQL queries of catalogs
  and revision references use the query's snapshot. Same-branch DDL takes
  PostgreSQL's lock levels on durable OIDs instead of preserving old schemas
  for open snapshots.
- A branch's identities always equal the tables and indexes of its working
  schema; they are synchronized whenever a working ref moves.
- Expedient for now: a database becomes versioned at its first prolly table
  (any storage binding), not at initdb, so heap-only databases are plain
  PostgreSQL and core regression passes. The goal remains versioned by
  default, with core regression passing on prolly storage.
- Native objects (heap tables, C functions, types) coexist unversioned.
  Native DDL may not write physical catalog rows for, or depending on,
  versioned objects; it fails cleanly instead.

Outstanding from this plan:

- Shared registry for leased revision OIDs. They are backend-local, which is
  harmless for locks but keeps revision relations out of parallel query.
- Statistics per durable OID. ANALYZE and VACUUM skip branch tables.
- Catalog consumer coverage: SQL catalog scans on a versioned database omit
  native objects (pg_class, \d, information_schema, dolt_statistics), lack
  system columns such as tableoid (pg_dump fails), and do not route by
  revision OIDs in their quals.

Versioned DDL that did not exist yet at this milestone, each needing a design
checkpoint. Most of it has since been implemented; see
[versioned schema objects](CATALOG-DDL-PLAN.md#versioned-schema-objects--2026-10-26)
for what remains:

- Ownership and ACLs (GRANT/REVOKE, OWNER TO, privilege checks). Role
  identities should likely stay out of versioned storage, like OIDs.
- CREATE TABLE features: defaults and CHECK inline (ALTER works), serial,
  identity, generated columns, reloptions, non-unique and expression indexes.
- Partitioned and inherited tables; temporary and unlogged prolly tables.
- User-defined types (enum, domain, composite, range) in versioned tables.
- Versioned views, procedures and triggers; routines with parsed bodies or
  argument defaults.
- CREATE TABLE AS, SELECT INTO, SET ACCESS METHOD to prolly, RENAME.
- pg_dump and pg_restore of a repository.
