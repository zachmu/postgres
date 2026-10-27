# Rows decode through a plan built once per relation. Every kind of column
# the codec handles must still decode correctly, under a client encoding that
# differs from the database's, and the plan must follow schema changes.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('decode_plan');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nmax_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sql(q{CREATE TABLE seed(id int); CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy'); CREATE TYPE pair AS (a int, b text); CREATE DOMAIN posint AS int CHECK (VALUE > 0);
 CREATE TABLE t(id int PRIMARY KEY, b bool, s smallint, big bigint, f4 real, f8 double precision, n numeric(12,4), c char(3), vc varchar(20), tx text, by bytea,
   d date, tm time, ts timestamp, tz timestamptz, iv interval, u uuid, j json, jb jsonb, m mood, p pair, pi posint, ia int[], ta text[], xm xml);
 INSERT INTO t VALUES (1, true, 7, 9000000000, 1.5, 2.25, 1234.5678, 'ab', 'héllo', 'wörld ✓', '\xdeadbeef', '2024-02-29', '13:14:15', '2024-02-29 13:14:15', '2024-02-29 13:14:15+00', '1 day 2 hours', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', '{"k": [1, 2]}', '{"k": "v"}', 'happy', ROW(3, 'x'), 5, '{1,2,3}', '{"a","b"}', '<a>1</a>'),
   (2, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)});
my $row1 = '1|t|7|9000000000|1.5|2.25|1234.5678|ab |héllo|wörld ✓|\xdeadbeef|2024-02-29|13:14:15|2024-02-29 13:14:15|2024-02-29 13:14:15+00|1 day 02:00:00|a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11|{"k": [1, 2]}|{"k": "v"}|happy|(3,x)|5|{1,2,3}|{a,b}|<a>1</a>';
is(sql(q{SET timezone='UTC'; SELECT * FROM t WHERE id = 1}), $row1, 'every column kind decodes');
is(sql(q{SELECT * FROM t WHERE id = 2}), '2|f' . ('|' x 23), 'nulls decode');
is(sql(q{SET timezone='UTC'; SET client_encoding='LATIN1'; SELECT id, c, vc, n, d FROM t WHERE id = 1}), "1|ab |h\xe9llo|1234.5678|2024-02-29", 'a different client encoding decodes stored UTF-8 and converts on output');
is(sql(q{SET client_encoding='LATIN1'; SELECT octet_length(vc), length(vc) FROM t WHERE id = 1}), '6|5', 'stored bytes are decoded as database encoding under a different client encoding');
is(sql(q{SELECT sum(id), count(ts), max(vc) FROM t}), '3|1|héllo', 'aggregates over decoded rows');

# The plan follows the relation's descriptor.
sql(q{ALTER TABLE t ADD COLUMN extra numeric DEFAULT 0.5; UPDATE t SET extra = id * 1.25});
is(sql(q{SELECT id, extra FROM t ORDER BY id}), "1|1.25\n2|2.50", 'added column decodes after the plan is rebuilt');
sql(q{ALTER TABLE t DROP COLUMN vc});
is(sql(q{SELECT id, tx, extra FROM t ORDER BY id}), "1|wörld ✓|1.25\n2||2.50", 'dropped column leaves the others decoding');

# A warm session keeps decoding correctly across statements and transactions.
my $s = $node->background_psql('postgres');
is($s->query_safe(q{SELECT count(*), max(n) FROM t}), '2|1234.5678', 'warm session, first statement');
$s->query_safe(q{INSERT INTO t(id, n, c) VALUES (3, 99.5, 'z')});
is($s->query_safe(q{SELECT id, n, c FROM t WHERE id = 3}), '3|99.5000|z  ', 'warm session, after an insert');
is($s->query_safe(q{BEGIN; ALTER TABLE t ADD COLUMN late text DEFAULT 'd'; SELECT late FROM t WHERE id = 3; ROLLBACK}), 'd', 'plan rebuilt inside a transaction after ALTER');
is($s->query_safe(q{SELECT id, c FROM t WHERE id = 3}), '3|z  ', 'and restored after rollback');
$s->quit;
$node->stop;
done_testing();
