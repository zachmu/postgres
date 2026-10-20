import unittest
from publication import Publication, Snapshot, digest


class PublicationTests(unittest.TestCase):
    def records(self):
        a, b = digest(b'a'), digest(b'b')
        return [('chunk', 10, (a, b'a')), ('chunk', 10, (b, b'b')),
                ('propose', 10, (('main', None, a), ('dev', None, b))),
                ('prepare', 10, None), ('commit', 10, None)]

    def test_every_durable_prefix(self):
        records = self.records()
        for cut in range(len(records) + 1):
            with self.subTest(cut=cut):
                p = Publication()
                for record in records[:cut]:
                    p.append(record)
                p.flush()
                for record in records[cut:]:
                    p.append(record)
                recovered = p.restart()
                self.assertEqual(len(recovered.refs), 2 if cut == 5 else 0)
                self.assertEqual(recovered.prepared, {10} if cut == 4 else set())
                self.assertEqual(recovered.refs, recovered.restart().refs)
                if cut == 4:
                    recovered.append(('commit', 10, None))
                    recovered.flush()
                    self.assertEqual(len(recovered.restart().refs), 2)

    def test_abort_and_snapshot(self):
        p = Publication()
        for r in self.records()[:3]:
            p.append(r)
        p.append(('abort', 10, None))
        p.flush()
        self.assertEqual(p.restart().refs, {})
        q = Publication()
        for r in self.records():
            q.append(r)
        self.assertEqual(q.snapshot_refs(Snapshot(11, frozenset({10}))), {})
        self.assertEqual(q.snapshot_refs(Snapshot(10, frozenset())), {})
        self.assertEqual(q.snapshot_refs(Snapshot(11, frozenset())), q.refs)

    def test_missing_content_and_atomic_validation(self):
        p = Publication()
        with self.assertRaises(ValueError):
            p.append(('propose', 1, (('main', None, 'absent'),)))
        self.assertEqual(p.log, [])
        for r in self.records()[:3]:
            p.append(r)
        root = digest(b'a')
        p.append(('propose', 11, (('dev', None, root),)))
        p.append(('commit', 11, None))
        with self.assertRaises(ValueError):
            p.append(('commit', 10, None))
        self.assertNotIn('main', p.refs)
        self.assertEqual(p.refs, {'dev': root})

    def test_corruption_and_duplicate_ref(self):
        p = Publication()
        with self.assertRaises(ValueError):
            p.append(('chunk', 1, (digest(b'a'), b'corrupted')))
        p.append(('chunk', 1, (digest(b'a'), b'a')))
        with self.assertRaises(ValueError):
            p.append(('propose', 1, (('x', None, digest(b'a')),
                                     ('x', None, digest(b'a')))))
