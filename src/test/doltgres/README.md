# Transaction publication specification

The prolly storage prototype has moved to the standalone C library at
`src/doltgres/prolly`. Build and run its native tests from the repository root:

```
make -C src/doltgres/prolly check
make -C src/doltgres/prolly sanitize
```

This directory now contains only the independent Python transaction-publication
model and its tests. It is not a prolly tree or storage engine. Run its four
model tests with:

```
python3 -m unittest discover -s src/test/doltgres -v
```

Those tests enumerate durable abstract record prefixes. They do not test real
fsync/WAL, implement PostgreSQL's transaction manager, or participate in the C
library build. The former Python tree, canonical SQL/schema encoder, table/index
model and their tests were removed. SQL type serialization and table/index-AM
integration will be implemented separately from the generic C tree.

See `doc/doltgres/contracts.md` for the design invariants and
`src/doltgres/prolly/README.md` for the actual C format and supported operations.
