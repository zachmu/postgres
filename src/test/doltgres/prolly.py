"""Persistent content-addressed ordered map; executable format v1 prototype.

Updates materialize ordered entries in memory, but rechunk only from the first
changed chunk to a resynchronization boundary at each level. They reuse suffix
nodes without serializing them. Production cursors/IO are phase 2 work.
"""
from bisect import bisect_left
from dataclasses import dataclass
from hashlib import sha256
import struct
from canonical import frame

MIN_ITEMS = 2
MAX_ITEMS = 16


@dataclass(frozen=True)
class Node:
    level: int
    entries: tuple

    def encode(self):
        if not 0 <= self.level <= 255 or len(self.entries) > MAX_ITEMS:
            raise ValueError('invalid node shape')
        if any(a[0] >= b[0] for a, b in zip(self.entries, self.entries[1:])):
            raise ValueError('keys are not strictly increasing')
        if self.level and any(len(v) != 32 for _, v in self.entries):
            raise ValueError('invalid child address')
        return (b'DG-node-v1\0' + bytes([self.level]) +
                struct.pack('>I', len(self.entries)) +
                b''.join(frame(k) + frame(v) for k, v in self.entries))

    @classmethod
    def decode(cls, data):
        magic = b'DG-node-v1\0'
        if not data.startswith(magic) or len(data) < len(magic) + 5:
            raise ValueError('invalid node header')
        offset = len(magic)
        level = data[offset]
        count = struct.unpack_from('>I', data, offset + 1)[0]
        offset += 5
        if count > MAX_ITEMS:
            raise ValueError('invalid item count')
        fields = []
        for _ in range(2 * count):
            if offset + 4 > len(data):
                raise ValueError('truncated length')
            length = struct.unpack_from('>I', data, offset)[0]
            offset += 4
            if offset + length > len(data):
                raise ValueError('truncated field')
            fields.append(data[offset:offset + length])
            offset += length
        if offset != len(data):
            raise ValueError('trailing node bytes')
        result = cls(level, tuple(zip(fields[::2], fields[1::2])))
        result.encode()  # Validate ordering and child addresses.
        return result


def boundary(level, entries):
    size = len(entries)
    if size < MIN_ITEMS:
        return False
    key = entries[-1][0]
    fingerprint = sha256(b'DG-boundary-v1\0' + bytes([level]) + key).digest()
    return size >= MAX_ITEMS or fingerprint[0] & 7 == 0


class Store:
    def __init__(self):
        self.chunks = {}
        self.serialized = 0

    def put(self, node):
        data = node.encode()
        address = sha256(data).digest()
        self.serialized += 1
        if address in self.chunks and self.chunks[address] != data:
            raise ValueError('hash collision')
        self.chunks[address] = data
        return address

    def read(self, address):
        data = self.chunks[address]
        if sha256(data).digest() != address:
            raise ValueError('corrupt content')
        return Node.decode(data)

    def reachable(self, roots):
        seen = set()
        todo = list(roots)
        while todo:
            address = todo.pop()
            if address in seen:
                continue
            seen.add(address)
            node = self.read(address)
            if node.level:
                todo.extend(child for _, child in node.entries)
        return seen

    def collect(self, roots):
        retained = self.reachable(roots)
        self.chunks = {k: v for k, v in self.chunks.items() if k in retained}


def rechunk(store, entries, level, old):
    """Reuse prefix and resynchronized suffix, including when tree height changes."""
    result = []
    offset = 0
    old_data = [(address, store.read(address)) for address in old]
    for address, node in old_data:
        end = offset + len(node.entries)
        if (tuple(entries[offset:end]) != node.entries or
                (end < len(entries) and not boundary(level, node.entries))):
            break
        result.append(address)
        offset = end
    else:
        if result and offset == len(entries):
            return result
    old_flat = tuple(pair for _, node in old_data for pair in node.entries)
    old_offsets = {}
    end = 0
    for index, (address, node) in enumerate(old_data):
        end += len(node.entries)
        old_offsets[address] = (index, end)
    pending = []
    while offset < len(entries):
        pending.append(entries[offset])
        offset += 1
        if boundary(level, pending) or offset == len(entries):
            address = store.put(Node(level, tuple(pending)))
            result.append(address)
            pending = []
            if address in old_offsets:
                index, end = old_offsets[address]
                if tuple(entries[offset:]) == old_flat[end:]:
                    result.extend(old[index + 1:])
                    break
    if not result:
        result.append(store.put(Node(0, ())))
    return result


class Tree:
    def __init__(self, store, root):
        self.store = store
        self.root = root

    @classmethod
    def build(cls, store, pairs=()):
        pairs = sorted(pairs)
        if any(a[0] == b[0] for a, b in zip(pairs, pairs[1:])):
            raise ValueError('duplicate map key')
        return cls._construct(store, pairs, {})

    @classmethod
    def _construct(cls, store, entries, old_levels):
        level = 0
        while True:
            nodes = rechunk(store, entries, level, old_levels.get(level, []))
            if len(nodes) == 1:
                return cls(store, nodes[0])
            entries = [(store.read(address).entries[-1][0], address)
                       for address in nodes]
            level += 1

    def levels(self):
        levels = {}
        todo = [self.root]
        while todo:
            next_level = []
            for address in todo:
                node = self.store.read(address)
                levels.setdefault(node.level, []).append(address)
                if node.level:
                    next_level.extend(child for _, child in node.entries)
            todo = next_level
        return levels

    def items(self):
        return [pair for address in self.levels()[0]
                for pair in self.store.read(address).entries]

    def get(self, key):
        address = self.root
        while True:
            node = self.store.read(address)
            offset = bisect_left([k for k, _ in node.entries], key)
            if offset == len(node.entries):
                return None
            candidate, value = node.entries[offset]
            if not node.level:
                return value if candidate == key else None
            address = value

    def apply(self, edits):
        # None means remove; b'' is a legitimate value.
        pairs = dict(self.items())
        for key, value in edits:
            if value is None:
                pairs.pop(key, None)
            else:
                pairs[key] = value
        return self._construct(self.store, sorted(pairs.items()), self.levels())
