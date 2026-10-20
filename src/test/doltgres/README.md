# Doltgres executable specifications

Run from any directory:

```
python3 -m unittest discover -s src/test/doltgres -v
```

Python 3.10+ standard library only. This directory holds an executable format
and publication oracle for phases 0–1, not a PostgreSQL storage implementation.
A deliberately independent oracle allows future native code to be checked
against deterministic fixtures instead of duplicating its implementation.
Publication crash tests enumerate durable abstract record prefixes; they do
not test real fsync/WAL or claim to implement PostgreSQL's transaction manager.
See `doc/doltgres/contracts.md` for scope and production obligations.

Storage experiment
------------------

`canonical.py` defines framed values and symbolic schema encodings. `prolly.py`
implements immutable SHA-256 addressed nodes with content-defined chunking and
incremental rechunking/resynchronization at every level. `table.py` constructs
canonical table and index roots together. The keyless multiplicity choice is
an experiment, not a final tuple identity design.

Updates currently materialize ordered entries and rebuild the logical index
input in memory. They reuse unchanged node prefixes/suffixes without reserializing
them, but are not O(log N) cursor updates. No disk chunk store, SQL table/index
AM, WAL IO, or broad PostgreSQL type support is implemented by these modules.
The Python oracle is intended to remain independent of future native storage.

All tree comparisons are lexicographic encoded-byte comparisons. This is not a
claim of SQL ordering support for framed text, numeric, arbitrary operator
classes, or composite keys. Equality/canonicalization and generic tree shape
are the phase 1 subjects; native SQL index ordering belongs in phase 2.

To include real-server equality tests (rather than skip them):

```
DOLTGRES_TEST_DSN='host=/tmp/doltgres-sockets port=55432 dbname=postgres' \
PSQL=/workspace/install/doltgres/bin/psql \
python3 -m unittest discover -s src/test/doltgres -v
```

Use a disposable database. Tests create temporary tables only. Standard-library
unit tests include deterministic random histories, fresh/incremental equality,
growth/shrink boundaries, old-root persistence, localized chunk reuse, corrupted
content rejection, retained-root GC, duplicate rows, unique NULL behavior,
versioned index contents and runtime identity exclusion.
