# A forward sequential scan walks the content and key trees in step; backward
# scans and cursors fall back to materialized handles; index fetches reuse one
# state per scan. Results must match a heap table in every case.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('scan_walk');
$node->init;
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sql(q{CREATE TABLE p(id int, k int, v text) USING dolt_prolly_table; CREATE TABLE h(id int, k int, v text) USING heap;
 INSERT INTO p SELECT g, g % 37, 'v' || (g % 100) FROM generate_series(1, 5000) g;
 INSERT INTO p VALUES (1, 1, 'v1'), (1, 1, 'v1'), (2, 2, 'v2');
 INSERT INTO h SELECT * FROM p; CREATE INDEX pk ON p(k); CREATE INDEX hk ON h(k); CREATE TABLE e(id int) USING dolt_prolly_table});
sub same { my ($q, $label) = @_; (my $hq = $q) =~ s/\bp\b/h/g; is(sql($q), sql($hq), $label); }
same(q{SELECT count(*), sum(id), sum(k), count(DISTINCT v) FROM p}, 'forward scan sees every row once');
same(q{SELECT id, v, count(*) FROM p GROUP BY id, v HAVING count(*) > 1 ORDER BY id}, 'duplicate rows are each a row');
same(q{SELECT count(*) FROM p WHERE v = 'v1'}, 'quals over the streamed rows');
is(sql(q{SELECT count(*) FROM e}), '0', 'empty table');
same(q{SELECT count(*) FROM (SELECT DISTINCT ctid FROM p) x}, 'every row has a distinct TID');
same(q{SELECT count(*) FROM p a JOIN p b ON a.k = b.k AND a.id < 10}, 'rescanned inner side');
same(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT sum(id) FROM p WHERE k IN (3, 5, 7)}, 'index fetches through one held state');
same(q{SET enable_seqscan=off; SELECT count(*) FROM p WHERE k BETWEEN 10 AND 20}, 'bitmap scan');
is(sql(q{BEGIN; DECLARE c SCROLL CURSOR FOR SELECT id FROM p ORDER BY id; FETCH 3 FROM c; FETCH BACKWARD 2 FROM c; FETCH ABSOLUTE 5003 FROM c; COMMIT}), "1\n1\n1\n1\n1\n5000", 'cursor moving backward over a sorted scan');
is(sql(q{BEGIN; DECLARE c SCROLL CURSOR FOR SELECT count(*) OVER () FROM p; FETCH 1 FROM c; FETCH BACKWARD 1 FROM c; FETCH FORWARD 2 FROM c; COMMIT}), "5003\n5003\n5003", 'scrolling the scan itself');
is(sql(q{BEGIN; INSERT INTO p VALUES (9001, 1, 'new'); SELECT count(*) FROM p WHERE id = 9001; UPDATE p SET v = 'newer' WHERE id = 9001; SELECT v FROM p WHERE id = 9001; DELETE FROM p WHERE id = 9001; SELECT count(*) FROM p WHERE id = 9001; COMMIT}), "1\nnewer\n0", 'scans within a transaction follow its own writes');
sql(q{DELETE FROM p WHERE id % 2 = 0; DELETE FROM h WHERE id % 2 = 0});
same(q{SELECT count(*), sum(id) FROM p}, 'scan after deletes');
same(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT count(*), sum(id) FROM p WHERE k = 1}, 'index fetches after deletes');
# Parsed nodes survive a transaction; an aborted transaction's own do not.
my $s = $node->background_psql('postgres');
is($s->query_safe(q{SELECT count(*), sum(id) FROM p}), sql(q{SELECT count(*), sum(id) FROM h}), 'warm session, first scan');
$s->query_safe(q{BEGIN; INSERT INTO p SELECT g, 1, 'x' FROM generate_series(20001, 20500) g; ROLLBACK});
is($s->query_safe(q{SELECT count(*), sum(id) FROM p}), sql(q{SELECT count(*), sum(id) FROM h}), 'scan after an aborted insert');
$s->query_safe(q{INSERT INTO p SELECT g, 1, 'x' FROM generate_series(20001, 20500) g});
sql(q{INSERT INTO h SELECT g, 1, 'x' FROM generate_series(20001, 20500) g});
is($s->query_safe(q{SELECT count(*), sum(id) FROM p}), sql(q{SELECT count(*), sum(id) FROM h}), 'scan after a committed insert in the same session');
is(sql(q{SELECT count(*), sum(id) FROM p}), sql(q{SELECT count(*), sum(id) FROM h}), 'and from another session');
$s->quit;
$node->stop;
done_testing();
