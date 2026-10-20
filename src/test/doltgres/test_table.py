from dataclasses import replace
import random
import unittest
from canonical import Column, Index, Schema
from table import Table


class TableTests(unittest.TestCase):
    def schema(self, keyed=True):
        return Schema('public', 'accounts',
                      (Column('id', 'int8'), Column('balance', 'numeric')),
                      primary=(0,) if keyed else (),
                      indexes=(Index('balance_idx', (1,)),))

    def test_distinct_histories_and_runtime_identifiers(self):
        schema = self.schema()
        rows = [(n, str(n * 100) + '.000') for n in range(90)]
        first = Table(schema, runtime={'oid': 123, 'xid': 500}).edit(inserts=rows)
        random.Random(17).shuffle(rows)
        second = Table(schema, runtime={'oid': 987, 'xid': 999})
        for row in rows:
            second = second.edit(inserts=[row])
        self.assertEqual(first.content_hash, second.content_hash)
        changed = second.edit(deletes=[(10, '1000')], inserts=[(10, '1111')])
        restored = changed.edit(deletes=[(10, '1111')], inserts=[(10, '1000')])
        self.assertEqual(restored.content_hash, first.content_hash)
        self.assertNotEqual(changed.indexes['balance_idx'].root,
                            first.indexes['balance_idx'].root)
        self.assertEqual(first.rows[first.reference((10, '1000'))][1], 1)

    def test_duplicate_occurrences_and_nulls(self):
        schema = self.schema(keyed=False)
        a = Table(schema).edit(inserts=[(1, '2.0'), (1, '2'), (None, None)])
        b = Table(schema).edit(inserts=[(None, None), (1, '2'), (1, '2.000')])
        self.assertEqual(a.content_hash, b.content_hash)
        c = a.edit(deletes=[(1, '2')])
        self.assertNotEqual(a.content_hash, c.content_hash)
        self.assertEqual(c.content_hash,
                         Table(schema).edit(inserts=[(1, '2'), (None, None)]).content_hash)
        self.assertEqual(len(a.data.items()), 2)
        self.assertEqual(len(a.indexes['balance_idx'].items()), 2)

    def test_constraints_and_index_definition(self):
        schema = replace(self.schema(), indexes=(Index('balance_idx', (1,), True),))
        original = Table(schema).edit(inserts=[(1, '2')])
        root = original.content_hash
        for values in [(1, '3'), (2, '2.00')]:
            with self.assertRaises(ValueError):
                original.edit(inserts=[values])
        self.assertEqual(original.content_hash, root)
        original.edit(inserts=[(2, None), (3, None)])  # Default UNIQUE NULLS DISTINCT.
        different = Table(replace(schema, indexes=())).edit(inserts=[(1, '2')])
        self.assertNotEqual(original.content_hash, different.content_hash)

    def test_schema_reference_changes_and_retention(self):
        schema = self.schema()
        table = Table(schema).edit(inserts=[(1, '2')])
        renamed = Table(replace(schema, name='renamed')).edit(inserts=[(1, '2')])
        self.assertNotEqual(table.content_hash, renamed.content_hash)
        self.assertEqual(table.data.root, renamed.data.root)
        table.store.collect(table.retention_roots())
        self.assertEqual(len(table.data.items()), 1)
        self.assertEqual(len(table.indexes['balance_idx'].items()), 1)
