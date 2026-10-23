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
