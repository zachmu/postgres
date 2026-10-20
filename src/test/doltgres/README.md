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
