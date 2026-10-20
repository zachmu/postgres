"""Canonical table/index root experiment; no SQL execution or MVCC here."""
from hashlib import sha256
import struct
from canonical import frame
from prolly import Store, Tree


class Table:
    def __init__(self, schema, store=None, runtime=None):
        schema.encode()  # Fail closed for unsupported schema encodings.
        self.schema = schema
        self.store = Store() if store is None else store
        self.runtime = {} if runtime is None else dict(runtime)
        self.rows = {}  # canonical reference -> (logical values, multiplicity)
        self.data = Tree.build(self.store)
        self.indexes = {i.name: Tree.build(self.store) for i in schema.indexes}

    def reference(self, values):
        row = self.schema.row(values)
        return self.schema.key(values, self.schema.primary) if self.schema.primary else row

    def edit(self, inserts=(), deletes=()):
        rows = dict(self.rows)
        for values in deletes:
            reference = self.reference(values)
            if reference not in rows:
                raise ValueError('missing row')
            old, count = rows[reference]
            if self.schema.row(old) != self.schema.row(values):
                raise ValueError('delete values do not match')
            if count == 1:
                del rows[reference]
            else:
                rows[reference] = (old, count - 1)
        for values in inserts:
            values = tuple(values)
            reference = self.reference(values)
            count = rows[reference][1] if reference in rows else 0
            if self.schema.primary and count:
                raise ValueError('duplicate primary key')
            rows[reference] = (values, count + 1)

        index_pairs = {}
        for index in self.schema.indexes:
            pairs = {}
            unique = set()
            for reference, (values, count) in rows.items():
                indexed = self.schema.key(values, index.columns)
                if index.unique and all(values[n] is not None for n in index.columns):
                    if indexed in unique or count > 1:
                        raise ValueError('duplicate unique key')
                    unique.add(indexed)
                pairs[frame(indexed) + frame(reference)] = struct.pack('>Q', count)
            index_pairs[index.name] = pairs

        # Validate all constraints before constructing the new persistent snapshot.
        result = object.__new__(Table)
        result.schema = self.schema
        result.store = self.store
        result.runtime = dict(self.runtime)
        result.rows = rows
        pairs = {reference: frame(self.schema.row(values)) + struct.pack('>Q', count)
                 for reference, (values, count) in rows.items()}
        result.data = self._update(self.data, pairs)
        result.indexes = {name: self._update(self.indexes[name], pairs)
                          for name, pairs in index_pairs.items()}
        return result

    @staticmethod
    def _update(tree, pairs):
        old = dict(tree.items())
        edits = [(key, None) for key in old.keys() - pairs.keys()]
        edits.extend((key, value) for key, value in pairs.items() if old.get(key) != value)
        return tree.apply(edits)

    @property
    def content_hash(self):
        # Metadata bytes deliberately exclude runtime mappings and transaction IDs.
        data = (b'DG-table-v1\0' + frame(self.schema.encode()) + self.data.root +
                b''.join(frame(name.encode()) + tree.root
                         for name, tree in sorted(self.indexes.items())))
        return sha256(data).hexdigest()

    def retention_roots(self):
        # Metadata edges are explicit: leaf values are not blindly interpreted as hashes.
        return [self.data.root] + [tree.root for tree in self.indexes.values()]
