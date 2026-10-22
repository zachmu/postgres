# Canonical schema provider: first writable checkpoint

Implemented 2026-10-22 on `zachmu/catalog-schema-provider`. This advances option 1;
it does not complete the branch-aware catalog gate.

## Stored representation

A retained table descriptor references content and schema objects. New `schema`
and `namespace` objects have payload `DGSCHEMA2` and exactly one child: a retained
prolly chunk root. The tree maps each top-level JSON property name to canonical
JSON value text. Entries sort by key using C ordering, and keys/values are encoded
in UTF-8. Nested column/index/constraint lists are values, not separate per-column
keys. The existing prolly node codec and retained chunk store are reused; there
is no separate schema file, WAL stream or publication protocol.

Readers accept old zero-child JSON schema objects. One-child envelopes require
the marker and a chunk child. The provider reconstructs JSON for existing binding
code. Table and namespace readers both use it. Canonical definitions exclude
physical relation OIDs and PostgreSQL transaction metadata. The existing schema
digest in a member remains the digest of normalized definition JSON; it is not
the schema envelope address. DGDB1 remains the database descriptor format and
still duplicates some definition metadata. This is not a single global catalog
prolly tree or a completed versioned dependency graph.

## Branch-local DROP

After branch views are activated, `DROP TABLE ... RESTRICT` is intercepted by the
native utility path. It resolves names under the caller's search path, checks
ownership, locks the workspace, captures current selected-branch content, and
writes a successor database object excluding the named table and its indexes.
Only `working/<branch>` moves, with compare-and-swap under the SQL transaction.
Staged state and HEAD remain unchanged until add/commit. PostgreSQL WAL and
transaction outcome cover retained chunks, refs and captured root records.

Physical pg_class/pg_attribute/index rows and storage survive. Other branches and
retained commits can still use them. Abort, savepoint rollback and crash rollback
leave the old working descriptor visible. A reader with an old SQL snapshot keeps
its old membership; a fresh snapshot rejects the dropped table. Ordinary parser
lookup and the AM both enforce membership, including COPY. Prepared ordinary
prolly queries record manifest dependency and rebind even for zero-row plans.
Explicit working-branch references bind the working database descriptor first.

Capture filters physical tables/indexes by canonical membership before touching
roots, so add/commit cannot resurrect removed objects. Checkout allows additional
retained physical objects but requires target members to have compatible physical
definitions. It does not project a schema or recreate physical objects.

## Security and locking findings

The caller's identity is restored on success/error. Internal capture uses a fixed
`pg_catalog, pg_temp` search path; a real-server test creates temporary catalog-name
shadows before branch activation and executes DROP as a non-superuser owner.

An initial unconditional database-object exclusive lock caused ordinary PostgreSQL
parallel regression to wait behind a prepared transaction. Exclusive workspace
locking now occurs only on the activated branch path; the full regression suite
passes. Branch DDL itself still serializes through the existing broad workspace
lock. This is not the intended production concurrency design.

## Validation

- Assert-enabled Meson build: all 18 selected native test groups pass, including
  the 231-test PostgreSQL regression suite, test_prolly and test_doltgres.
- `013_schema_tree.pl`: 15 assertions, including deterministic identity across
  OID allocation and UTF-8/LATIN1 databases, legacy/new mixed reads, cold read-only
  decoding without an assigned XID, aborted capture and restart retention.
- `014_branch_drop.pl`: 28 assertions, including ownership, shadowing, transaction
  and savepoint rollback, old snapshot, prepared empty plan, COPY, branch isolation,
  historical access, commit capture, restart and crash-aborted publication.
- Fork Go compatibility: 114 assertions across nine focused suites: TestAsOf,
  TestCatalogHistory, TestNativeVersionedQueries, TestNativeHistoricalCursors,
  TestRevisionWorkingSets, TestDoltMergeData, TestPostgresMergeConflictStatus,
  TestProllyVersionCompatibility and TestBranchCatalogDrop.
- Doltgres reference: 61 assertions across the first five revision suites above
  plus TestBranchCatalogDrop. That last suite is a new 17-assertion shared scenario;
  it is not an existing disabled test made to pass. No assertions in these runs
  were skipped. These runs are not full Doltgres-suite conformance.

## Limits and next gate

Raw pg_catalog, regclass and information_schema still expose physical retained
objects. Definitions for ordinary capture still come from shared catalogs. Thus
membership is branch-authoritative, but the general catalog definition interface
is not. Layout-changing ALTER and CREATE after activation remain guarded, as do
unsupported custom type/function definitions. DROP CASCADE and schema merge remain
unsupported; this narrow path is not a general replacement for PostgreSQL's
DDL dependency/event-trigger machinery. Mutation retains the existing Read
Committed restriction. There is no new garbage collection or namespace/schema
cache; schema reads import the schema tree and namespace lookup scans retained
namespace objects.

The next experiment should add explicit writable branch/schema-generation binding
and branch-local CREATE, including same-name creation on separate branches. Then
prove ADD/DROP COLUMN with data rewrite, old snapshots/cursors and atomic abort.
Before declaring option 1 generally viable, resolve branch-aware catalog scans,
scoped dependencies and type/function runtime identities, and exercise incompatible
versions in one statement. Stable logical identity is desirable; reusing a physical
OID without version context through every relevant cache remains unsafe. This
checkpoint found no fundamental obstacle to option 1, but it does not settle those
remaining identity/cache questions.
