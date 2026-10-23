# Catalog milestone 4: native tuple proposals

CatalogTupleUpdate and CatalogTupleUpdateWithInfo can now route an update to an
explicit, revision-bound mutation sink. The sink is scoped with PG_TRY/PG_FINALLY;
it never changes the sealed source view. Calls outside a matching sink retain
the physical-access guard. The publisher owns branch locking, permissions,
canonical encoding and transactional ref updates.

ALTER COLUMN SET STATISTICS and SET/RESET column options now reuse PostgreSQL's
native validation and native pg_attribute tuple construction. The sink checks the
catalog, relation, attribute and every unchanged field. Unexpected changes fail
before publication. Multi-action statements in this metadata family reopen each
new immutable definition and refresh command visibility between actions; any
later error rolls the whole statement back. Other mixed ALTER families remain
unsupported. Native, unactivated heap DDL retains its original path.

## Catalog tuple payload experiment

The payload retains native values, not raw HeapTuple bytes. Statistics targets
are nullable integers; attribute options are text values in canonical ordering.
The schema property tree stores these fields alongside the existing symbolic
column definition. The provider reconstructs complete pg_attribute tuples,
including nullable fields beyond the fixed structure prefix. Native capture and
subsequent mutations use the same representation, with absent defaults omitted.
Tests change values and restore them, verifying an identical database root hash.
MVCC headers, synthetic TIDs, runtime OIDs and physical TOAST pointers are excluded.

This establishes a useful write boundary, not universal ALTER support. Arbitrary
native DDL still requires codecs for every affected catalog, dependency writes,
and provisional state for commands that reopen a changing definition within a
native multi-pass operation. Expression trees require symbolic identifier
translation. Multi-action support here is deliberately limited to metadata
commands whose sequential publications preserve native behavior. Statistics
metadata does not by itself implement branch-specific ANALYZE data collection.

## Tests

The native test covers initial native capture, metadata introspection, exact root
roundtrip, native errors, multi-action atomicity, transaction rollback, historical
catalogs, parallel branch writes, data preservation and restart. The Doltgres
harness adds the same branch-isolation and transaction scenarios. These native
metadata commands are additional fork coverage; no claim is made that current
Doltgres implements all their syntax. The complete existing compatibility gate
is also rerun, with original PostgreSQL regression outputs unchanged.

## General provisional mutation checkpoint

This supersedes the limited multi-action mechanism described above. Native ALTER
now runs its own passes against one private CatalogView. CatalogTupleInsert,
CatalogTupleUpdate and CatalogTupleDelete stage tuples; command-counter increments
rebuild visible rows and invalidate only that context's native caches. Unique
catalog identities and non-null fields are checked against pending writes too.
Savepoint rollback removes child changes. Existing immutable views, borrowed
catcache tuples and scans retain their definitions. Retirement defers decoded-row
reclamation until outstanding scans release their pins.

Supported ALTER families include column statistics/options/storage/compression,
NOT NULL, mixed ADD/DROP COLUMN, defaults, CHECK creation/deletion/validation and
non-indexed ALTER TYPE USING. Native ordering, permission/constraint checks and
expression evaluation are reused. Defaults and CHECKs use canonical SQL with
symbolic columns and normalized serialization settings; nodeToString output and
local OIDs exist only in reconstructed runtime tuples. USING keeps caller input
settings. Unsupported expressions reject before native planning/evaluation.

The optional relation_begin_rewrite table-AM callback preserves the original scan
root while inserts into the same contextual handle collect private replacement
rows. This lets ATRewriteTable evaluate defaults and USING, and validate rows,
without temporary heap relations or file swaps. It also materializes heap's
fast-default missing values. Final publication compacts dropped columns, installs
matching rows and rebuilds existing indexes under the same PostgreSQL transaction
as the working root. Internal updates may occur more than once; they become visible
atomically at transaction commit. Normal heap rewrite behavior is unchanged.

Native ALTER expressions execute as the invoking role. Internal storage access is
privileged only after target ownership authorization. Catalog mutation validators
reject unsupported defaults/CHECKs before later passes. A USING preflight examines
a copy of the parse tree: transformation must not modify the native controller's
input. Nested utility operations reject until their publication effects are modeled.

### Limits and next work

Independent index creation/deletion/replacement, indexed-column DROP/TYPE, foreign
keys, sequences/identity, generated columns, partitions and event triggers are not
admitted by this controller. Scalar expression support is an audited built-in node
set, not arbitrary user functions/types. Additional object codecs and shared-role
lifetime across retained history remain incomplete. Existing CREATE/DROP adapters
remain separate. This is a general staging and rewrite mechanism, not universal
PostgreSQL DDL coverage. Branch-specific ANALYZE, parallel-worker context transport,
handle/type-cache reclamation, persistent GC and row-level write concurrency remain
separate workstreams.

### Validation additions

The provisional suite covers command-visible mutations, mixed native passes,
constraints/defaults, original-caller evaluation, input/serialization settings,
late failures, cancellation, transaction/savepoint rollback, parallel branch DDL/DML,
parallel branch rewrites, historical queries and restart/crash recovery. Synthetic
catalog tests cover pinned readers, uniqueness, subtransactions and nested utility
rejection. The Doltgres gate now includes new atomic rewrite/history scenarios and
unchanged original DROP COLUMN, ADD IF NOT EXISTS, default, constraint and USING
cases. A harness fix makes SQLSTATE-only expected failures work correctly.
