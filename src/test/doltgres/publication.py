"""Executable publication/snapshot specification, not PostgreSQL WAL IO."""
from dataclasses import dataclass
from hashlib import sha256


def digest(data):
    return sha256(data).hexdigest()


@dataclass(frozen=True)
class Snapshot:
    xmax: int
    running: frozenset

    def sees(self, xid):
        return xid < self.xmax and xid not in self.running


class Publication:
    def __init__(self):
        self.log = []
        self.flushed = 0
        self.chunks = {}
        self.pending = {}
        self.prepared = set()
        self.decided = set()
        self.history = []
        self.refs = {}

    def append(self, record):
        # Validate before adding to the log; invalid proposals cannot replay.
        self._apply(record)
        self.log.append(record)

    def flush(self):
        self.flushed = len(self.log)

    def _apply(self, record):
        kind, xid, payload = record
        if kind == 'chunk':
            address, content = payload
            if digest(content) != address:
                raise ValueError('invalid chunk hash')
            self.chunks[address] = content
        elif kind == 'propose':
            if xid in self.pending or xid in self.decided:
                raise ValueError('duplicate transaction')
            # Tuple payloads are immutable, so client mutation cannot alter WAL.
            if len({ref for ref, _, _ in payload}) != len(payload):
                raise ValueError('duplicate ref')
            for ref, expected, root in payload:
                if self.refs.get(ref) != expected or root not in self.chunks:
                    raise ValueError('stale ref or missing content')
            self.pending[xid] = payload
        elif kind == 'prepare':
            if xid not in self.pending:
                raise ValueError('no proposal')
            self.prepared.add(xid)
        elif kind == 'commit':
            changes = self.pending[xid]
            for ref, expected, root in changes:
                if self.refs.get(ref) != expected:
                    raise ValueError('stale publication')
            # Validate all changes before exposing any of them.
            updates = {ref: root for ref, _, root in changes}
            self.refs = {**self.refs, **updates}
            self.history.append((xid, updates))
            self.pending.pop(xid)
            self.prepared.discard(xid)
            self.decided.add(xid)
        elif kind == 'abort':
            if xid in self.decided:
                raise ValueError('already decided')
            self.pending.pop(xid, None)
            self.prepared.discard(xid)
            self.decided.add(xid)
        else:
            raise ValueError('unknown record')

    def snapshot_refs(self, snapshot):
        refs = {}
        for xid, updates in self.history:
            if snapshot.sees(xid):
                refs.update(updates)
        return refs

    def restart(self):
        result = Publication()
        for record in self.log[:self.flushed]:
            result.append(record)
        # Unprepared proposals have no surviving owner after restart.
        result.pending = {x: p for x, p in result.pending.items()
                          if x in result.prepared}
        result.flush()
        return result
