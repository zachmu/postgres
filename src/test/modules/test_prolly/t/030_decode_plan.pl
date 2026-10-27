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

# Values the direct decoders must reproduce exactly: numeric specials and
# scales, float specials, date and timestamp infinities, padded multibyte
# bpchar, money.
sql(q{CREATE TABLE edge(id int PRIMARY KEY, n numeric, f8 double precision, f4 real, d date, ts timestamp, c char(4), vc varchar(10), mo money, u uuid, j json);
 SET timezone='UTC';
 INSERT INTO edge VALUES
  (1, 'NaN', 'NaN', 'Infinity', 'infinity', 'infinity', 'é', 'ab', '-12.34', '00000000-0000-0000-0000-000000000000', '[1, {"a": null}]'),
  (2, '-Infinity', '-Infinity', '-0', '-infinity', '-infinity', 'ü1', '', '922337203685.47', 'ffffffff-ffff-ffff-ffff-ffffffffffff', '"x"'),
  (3, '-0.000000000000000000001', '-0', '1.5e-45', '1899-12-31', '0001-01-01 00:00:00', '', 'ünïcødé', '0', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', 'null'),
  (4, '123456789012345678901234567890.123456789', '1.7976931348623157e308', '3.4028235e38', '2024-02-29', '294276-12-31 23:59:59', 'abcd', 'z', '1.01', NULL, '{}'),
  (5, '0', '0', '0', '2000-01-01', '2000-01-01 00:00:00', 'a', 'é', '0.01', NULL, '{"k":   "v"}'),
  (6, '1e100', '2.5e-310', '-1', '1000000-01-01', '1999-12-31 23:59:59.999999', 'ab', 'abcdefghij', '-0.01', NULL, '[]'),
  (7, '-999999.9999999999999', '-1.5', '-3.25', '0001-01-01', '2024-02-29 13:14:15.5', 'ab c', 'é é', '21474836.47', NULL, '1')});
my $edge = sql(q{SET timezone='UTC'; SELECT id, n, f8, f4, d, ts, c, octet_length(c), vc, mo, u, j FROM edge ORDER BY id});
is($edge, q{1|NaN|NaN|Infinity|infinity|infinity|é   |5|ab|-$12.34|00000000-0000-0000-0000-000000000000|[1, {"a": null}]
2|-Infinity|-Infinity|-0|-infinity|-infinity|ü1  |5||$922,337,203,685.47|ffffffff-ffff-ffff-ffff-ffffffffffff|"x"
3|-0.000000000000000000001|-0|1e-45|1899-12-31|0001-01-01 00:00:00|    |4|ünïcødé|$0.00|a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11|null
4|123456789012345678901234567890.123456789|1.7976931348623157e+308|3.4028235e+38|2024-02-29|294276-12-31 23:59:59|abcd|4|z|$1.01||{}
5|0|0|0|2000-01-01|2000-01-01 00:00:00|a   |4|é|$0.01||{"k":   "v"}
6|10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000|2.5e-310|-1|1000000-01-01|1999-12-31 23:59:59.999999|ab  |4|abcdefghij|-$0.01||[]
7|-999999.9999999999999|-1.5|-3.25|0001-01-01|2024-02-29 13:14:15.5|ab c|4|é é|$21,474,836.47||1}, 'edge values decode exactly');
is(sql(q{SELECT sum(n) FILTER (WHERE id IN (4, 5)), sum(f8) FILTER (WHERE id IN (3, 5, 7)), max(d), min(ts), count(DISTINCT c), sum(mo) FROM edge}),
	'123456789012345678901234567890.123456789|-1.5|infinity|-infinity|7|$922,358,678,510.61', 'decoded values compute');

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
