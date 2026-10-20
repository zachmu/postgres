# Standalone C prolly-tree prototype

Build with `make check` in this directory (C11 compiler, pkg-config and OpenSSL
libcrypto development headers). No configured PostgreSQL tree or server is
required. `make sanitize` runs AddressSanitizer and UndefinedBehaviorSanitizer.

The generic map uses unsigned lexicographic byte keys. Callers supply canonical,
order-preserving key encodings for their schema; a heap tuple's MVCC header,
runtime OIDs, padding, and TOAST pointers are not a canonical representation.
Leaves hold opaque tuple bytes, with a schema-driven tuple-packing helper.
Internal nodes hold each child's maximum key, SHA-256 address and row count.
Root copies create independent branches in O(1). The caller owns the store and
must keep it alive while any root or borrowed slice is in use.

This is an in-memory immutable chunk store with import/export, not a table AM,
WAL implementation, or durable publication service. Formats are experimental.
