from decimal import localcontext
import unittest
from canonical import Column, Schema, scalar


class CanonicalTests(unittest.TestCase):
    def test_numeric_equality_without_context_rounding(self):
        for left, right in [('1', '1.000'), ('0', '-0.00'), ('1000', '1e3'),
                            ('NaN', '-NaN')]:
            self.assertEqual(scalar('numeric', left), scalar('numeric', right))
        with localcontext() as ctx:
            ctx.prec = 2
            self.assertNotEqual(scalar('numeric', '12345678901234567890'),
                                scalar('numeric', '12345678901234567891'))

    def test_framing_and_int_order(self):
        schema = Schema('public', 't', (Column('a', 'text'), Column('b', 'text')))
        self.assertNotEqual(schema.row(('ab', 'c')), schema.row(('a', 'bc')))
        self.assertNotEqual(scalar('text', None), scalar('text', ''))
        values = [-(1 << 63), -1, 0, 1, (1 << 63) - 1]
        self.assertEqual(sorted(map(lambda v: scalar('int8', v), values)),
                         [scalar('int8', v) for v in values])

    def test_unsupported_and_invalid_inputs(self):
        for kind, value in [('oid', 1), ('int8', True), ('int8', 1 << 63),
                            ('text', 'a\0b'), ('bool', 1), ('bytea', 'abc')]:
            with self.assertRaises(ValueError):
                scalar(kind, value)
        with self.assertRaises(ValueError):
            Column('a', 'text', collation='nondeterministic').encode()
        with self.assertRaises(ValueError):
            Schema('p', 't', (Column('id', 'int8'),), primary=(0,)).row((None,))

    def test_format_fixture(self):
        self.assertEqual(scalar('int8', 0).hex(),
                         '0100000005696e743800000000088000000000000000')
        self.assertEqual(scalar('numeric', '1.00').hex(),
                         '01000000086e756d6572696300000000042b316530')
