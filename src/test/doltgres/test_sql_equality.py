"""Run against a disposable PostgreSQL 18 server using DOLTGRES_TEST_DSN.

Example: DOLTGRES_TEST_DSN='host=/tmp/doltgres-sockets port=55432 dbname=postgres'
PSQL=/workspace/install/doltgres/bin/psql python3 -m unittest discover -s src/test/doltgres -v
"""
import os
import subprocess
import unittest
from canonical import scalar


@unittest.skipUnless(os.getenv('DOLTGRES_TEST_DSN'), 'requires real PostgreSQL test DSN')
class SQLEqualityTests(unittest.TestCase):
    def query(self, query):
        return subprocess.check_output(
            [os.getenv('PSQL', 'psql'), '-XAt', '-v', 'ON_ERROR_STOP=1',
             '-d', os.environ['DOLTGRES_TEST_DSN'], '-c', query],
            text=True, env={**os.environ, 'LC_ALL': 'C'}).strip()

    def test_numeric_equivalence_and_non_equivalence(self):
        cases = [('1', '1.00'), ('0', '-0.00'), ('NaN', 'NaN'),
                 ('Infinity', 'Infinity'), ('-Infinity', 'Infinity'),
                 ('12345678901234567890', '12345678901234567891')]
        for a, b in cases:
            with self.subTest(a=a, b=b):
                # All literals are fixed test data, never externally supplied SQL.
                equal = self.query(f"SELECT '{a}'::numeric = '{b}'::numeric") == 't'
                self.assertEqual(scalar('numeric', a) == scalar('numeric', b), equal)

    def test_c_text_and_int_order(self):
        self.assertEqual(self.query("SELECT 'A'::text COLLATE \"C\" = 'a'::text"), 'f')
        self.assertNotEqual(scalar('text', 'A'), scalar('text', 'a'))
        self.assertEqual(self.query("SELECT string_agg(v::text, ',' ORDER BY v) FROM "
                                    "(VALUES (-1::bigint),(0),(1)) AS t(v)"), '-1,0,1')

    def test_keyless_duplicates_and_unique_nulls(self):
        result = self.query("BEGIN; CREATE TEMP TABLE dg_eq(v numeric UNIQUE); "
                            "INSERT INTO dg_eq VALUES(NULL),(NULL),(1); "
                            "SELECT count(*), count(v) FROM dg_eq; ROLLBACK;")
        self.assertIn('3|1', result.splitlines())
        self.assertEqual(self.query("SELECT count(DISTINCT v),count(*) FROM "
                                    "(VALUES(1::numeric),(1.00),(NULL)) AS t(v)"), '1|3')
