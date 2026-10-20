import random
import struct
import unittest
from prolly import Node, Store, Tree


def key(n):
    return struct.pack('>I', n)


class ProllyTests(unittest.TestCase):
    def test_history_independence_and_old_roots(self):
        rng = random.Random(1637)
        store = Store()
        tree = Tree.build(store)
        expected = {}
        for step in range(350):
            edits = [(key(rng.randrange(150)), None if rng.randrange(4) == 0
                      else key(rng.randrange(10000))) for _ in range(7)]
            before = tree.items()
            prior = tree
            tree = tree.apply(edits)
            for k, value in edits:
                if value is None:
                    expected.pop(k, None)
                else:
                    expected[k] = value
            with self.subTest(step=step):
                fresh = Tree.build(Store(), expected.items())
                self.assertEqual(tree.root, fresh.root)
                self.assertEqual(tree.items(), sorted(expected.items()))
                self.assertEqual(prior.items(), before)
                for k, v in edits:
                    self.assertEqual(tree.get(k), expected.get(k))

    def test_sharing_and_local_rechunking(self):
        store = Store()
        tree = Tree.build(store, ((key(n), b'v') for n in range(4096)))
        old = store.reachable([tree.root])
        store.serialized = 0
        changed = tree.apply([(key(2000), b'changed')])
        new = store.reachable([changed.root])
        self.assertGreater(len(old & new), len(old) * .95)
        self.assertLess(store.serialized, 20)
        self.assertEqual(changed.root, Tree.build(Store(), changed.items()).root)
        restored = changed.apply([(key(2000), b'v')])
        self.assertEqual(restored.root, tree.root)

    def test_height_changes_and_terminal_partial_chunks(self):
        store = Store()
        tree = Tree.build(store)
        for n in range(100):
            tree = tree.apply([(key(n), b'')])
            self.assertEqual(tree.root, Tree.build(Store(), tree.items()).root)
        self.assertGreater(store.read(tree.root).level, 0)
        for n in reversed(range(100)):
            tree = tree.apply([(key(n), None)])
            self.assertEqual(tree.root, Tree.build(Store(), tree.items()).root)
        self.assertEqual(tree.items(), [])

    def test_framing_corruption_and_duplicates(self):
        node = Node(0, ((b'a', b'b'),))
        encoded = node.encode()
        self.assertEqual(Node.decode(encoded), node)
        for cut in range(len(encoded)):
            with self.assertRaises(ValueError):
                Node.decode(encoded[:cut])
        with self.assertRaises(ValueError):
            Node.decode(encoded + b'extra')
        with self.assertRaises(ValueError):
            Tree.build(Store(), [(b'a', b'x'), (b'a', b'y')])
        store = Store()
        address = store.put(node)
        store.chunks[address] = encoded[:-1] + b'x'
        with self.assertRaises(ValueError):
            store.read(address)

    def test_retained_roots_survive_gc(self):
        store = Store()
        first = Tree.build(store, [(key(n), b'x') for n in range(100)])
        second = first.apply([(key(n), b'y') for n in range(100)])
        garbage = second.apply([(key(500), b'unreferenced')])
        store.collect([first.root, second.root])
        self.assertEqual(first.get(key(1)), b'x')
        self.assertEqual(second.get(key(1)), b'y')
        self.assertNotIn(garbage.root, store.chunks)
