# Private-namespace implementation withdrawn

2026-10-22. The private-namespace same-name CREATE milestone was rolled back at
the user's request. It resolved logical names to persisted physical catalog copies,
but left downstream catalog access dependent on context-free OIDs. Passing the
same-name tests did not establish the required branch-aware catalog architecture.

The rollback restores the runtime-mapping foundation preceding that milestone.
The private namespace and generation catalogs, resolver/deparser changes, and
associated implementation-specific native tests are removed. Same-name CREATE is
therefore not currently supported. The earlier foundation is not an endorsement of
physical schema copies as the final catalog architecture.

The shared Doltgres TestBranchCatalogSameName scenarios remain in the Doltgres
repository as acceptance tests for the replacement architecture. They are expected
to fail against this rolled-back fork; they have not been weakened or skipped.
Native tests from the withdrawn implementation remain available in Git history.

Future work must establish a coherent branch-aware interface for versioned catalog
access throughout planning, execution, caching, types, dependencies and SQL catalog
introspection. Canonical versioned schemas must be authoritative; unsupported
paths must not silently fall back to context-free physical catalog definitions.
This rollback does not implement that replacement architecture.

The catalog version is advanced to distinguish this layout from the withdrawn
implementation. Use a freshly initialized development cluster; no catalog migration
is provided. Root planning documents are unchanged.

Rollback validation: a fresh assert-enabled build passes all 21 selected native
suite groups (including 231 core regression tests and 122 isolation tests) and all
131 assertions in the ten previously supported Doltgres compatibility suites.
TestBranchCatalogSameName is deliberately outside this supported-slice run.
