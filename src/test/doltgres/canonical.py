"""Canonical v1 executable oracle. Never serialize runtime PostgreSQL identities."""
from dataclasses import dataclass
from decimal import Decimal
import struct

TYPES = frozenset(('bool', 'int8', 'text', 'bytea', 'numeric'))


def frame(data):
    return struct.pack('>I', len(data)) + data


def scalar(kind, value):
    if kind not in TYPES:
        raise ValueError('unsupported type: ' + kind)
    if value is None:
        return b'\x00'
    tag = kind.encode('ascii') + b'\0'
    if kind == 'bool':
        if type(value) is not bool:
            raise ValueError('expected bool')
        data = bytes([value])
    elif kind == 'int8':
        if type(value) is not int or not -(1 << 63) <= value < (1 << 63):
            raise ValueError('expected int8')
        data = struct.pack('>Q', value + (1 << 63))
    elif kind == 'text':
        if not isinstance(value, str) or '\0' in value:
            raise ValueError('expected PostgreSQL text')
        data = value.encode('utf-8')
    elif kind == 'bytea':
        if not isinstance(value, bytes):
            raise ValueError('expected bytes')
        data = value
    else:
        number = Decimal(value)
        if number.is_nan():
            data = b'NaN'
        elif number.is_infinite():
            data = b'-Infinity' if number.is_signed() else b'Infinity'
        elif not number:
            data = b'0'
        else:
            # Decimal.normalize() can round using ambient context. Never use it.
            sign, digits, exponent = number.as_tuple()
            digits = list(digits)
            while digits[-1] == 0:
                digits.pop()
                exponent += 1
            data = (('-' if sign else '+') + ''.join(map(str, digits)) +
                    'e' + str(exponent)).encode('ascii')
    return b'\x01' + frame(tag) + frame(data)


@dataclass(frozen=True)
class Column:
    name: str
    kind: str
    nullable: bool = True
    collation: str = 'C'

    def encode(self):
        if self.kind not in TYPES or self.collation != 'C':
            raise ValueError('unsupported type or collation')
        return (frame(self.name.encode()) + frame(self.kind.encode()) +
                bytes([self.nullable]) + frame(self.collation.encode()))


@dataclass(frozen=True)
class Index:
    name: str
    columns: tuple
    unique: bool = False

    def encode(self):
        return (frame(self.name.encode()) + bytes([self.unique]) +
                b''.join(struct.pack('>I', n) for n in self.columns))


@dataclass(frozen=True)
class Schema:
    namespace: str
    name: str
    columns: tuple
    primary: tuple = ()
    indexes: tuple = ()

    def encode(self):
        if len({c.name for c in self.columns}) != len(self.columns):
            raise ValueError('duplicate column')
        if len({i.name for i in self.indexes}) != len(self.indexes):
            raise ValueError('duplicate index')
        for positions in [self.primary] + [i.columns for i in self.indexes]:
            if len(set(positions)) != len(positions):
                raise ValueError('duplicate key column')
            if any(type(n) is not int or not 0 <= n < len(self.columns)
                   for n in positions):
                raise ValueError('invalid key column')
        return (b'DG-schema-v1\0bytewise-v1\0' + frame(self.namespace.encode()) +
                frame(self.name.encode()) + frame(b''.join(frame(c.encode())
                for c in self.columns)) + frame(b''.join(struct.pack('>I', n)
                for n in self.primary)) + frame(b''.join(frame(i.encode())
                for i in sorted(self.indexes, key=lambda i: i.name))))

    def row(self, values):
        if len(values) != len(self.columns):
            raise ValueError('wrong arity')
        fields = []
        for pos, (column, value) in enumerate(zip(self.columns, values)):
            if value is None and (not column.nullable or pos in self.primary):
                raise ValueError('null in non-null column')
            fields.append(frame(scalar(column.kind, value)))
        return b''.join(fields)

    def key(self, values, positions):
        return b''.join(frame(scalar(self.columns[n].kind, values[n]))
                        for n in positions)
