use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('bitmap');
$node->init;
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
# Identical schemas/values, but independent heap and prolly access methods.
sql(q{
 CREATE TABLE p(id int, k int, v text COLLATE "C") USING dolt_prolly_table;
 CREATE TABLE h(id int, k int, v text COLLATE "C") USING heap;
 INSERT INTO p SELECT g, CASE WHEN g%13=0 THEN NULL ELSE g%1000 END,
   repeat('payload',10) FROM generate_series(1,10000) g;
 INSERT INTO p VALUES (7,7,'duplicate'),(7,7,'duplicate');
 INSERT INTO h SELECT * FROM p;
 CREATE INDEX pi ON p(id);
 CREATE INDEX pk ON p(k DESC NULLS LAST) INCLUDE (v);
 CREATE INDEX hi ON h(id);
 CREATE INDEX hk ON h(k DESC NULLS LAST) INCLUDE (v);
});
my $force = 'SET enable_seqscan=off; SET enable_indexscan=off; SET enable_indexonlyscan=off; ';
for my $pred (
 'id IN (7,7,100,1000,NULL)',
 'id = ANY (ARRAY[]::int[])',
 'id = ANY (NULL::int[])',
 'id = ANY (ARRAY[NULL,NULL]::int[])',
 'id IN (7,100) OR id BETWEEN 95 AND 105',
 '(id BETWEEN 10 AND 20 OR id BETWEEN 15 AND 25) AND k IN (12,17,22)',
 'id IN (7,100,1000) OR k IN (7,17,NULL)',
 'k IS NULL OR k IN (7,17)',
 'k IN (7,17) AND id IN (7,17,1007,1017)',
 'id IN (7::bigint,100::bigint)',
 'id > ANY (ARRAY[9995,9998,NULL])',
 'id IN (7,100) AND id > 1000',
 "v='duplicate' AND k IN (7,17)") {
 my $q = "SELECT id,k,v, count(*) FROM %s WHERE $pred GROUP BY id,k,v ORDER BY id,k,v";
 is(sql($force . sprintf($q,'p')), sql(sprintf($q,'h')), "heap comparison: $pred");
}
for my $pred ('id IN (7,100,1000)', 'id BETWEEN 10 AND 15 OR id BETWEEN 100 AND 105') {
 like(sql("EXPLAIN (COSTS OFF) SELECT * FROM p WHERE $pred"), qr/Bitmap Index Scan/,
   "planner selects bitmap without forcing: $pred");
}
like(sql($force.'EXPLAIN (COSTS OFF) SELECT * FROM p WHERE id=7 OR k=17'), qr/BitmapOr/, 'OR combines indexes');
# Bitmap TIDs must retain the table AM's ordinary tuple identity.
is(sql($force.q{SELECT count(*)=count(DISTINCT ctid) FROM p WHERE id IN (7,7,100)}), 't', 'duplicate values have separate TIDs');
is(sql($force.q{WITH b AS MATERIALIZED (SELECT ctid,id FROM p WHERE id IN (7,100)) SELECT bool_and(p.id=b.id) FROM b JOIN p ON p.ctid=b.ctid}), 't', 'bitmap TIDs round trip through tuple fetch');
# Parameters, duplicate/null array members, and rescanning the same node.
is(sql($force.q{PREPARE q(int[]) AS SELECT count(*) FROM p WHERE id=ANY($1); EXECUTE q(ARRAY[7,7,NULL]); EXECUTE q(ARRAY[100]); EXECUTE q('{}');}), "3\n1\n0", 'prepared array rescans');
is(sql($force.q{SET enable_material=off; SELECT g,(SELECT count(*) FROM p WHERE id IN (g,g+1)) FROM generate_series(7,9) g ORDER BY g}), "7|4\n8|2\n9|2", 'correlated runtime-key rescans');
# Heap oracle for UPDATE/DELETE driven by the new access path.
sql($force.q{UPDATE p SET v='updated' WHERE id IN (7,100,1000); DELETE FROM p WHERE id BETWEEN 15 AND 20 OR k=17;});
sql(q{UPDATE h SET v='updated' WHERE id IN (7,100,1000); DELETE FROM h WHERE id BETWEEN 15 AND 20 OR k=17;});
is(sql('SELECT count(*) FROM ((TABLE p EXCEPT ALL TABLE h) UNION ALL (TABLE h EXCEPT ALL TABLE p)) d'), '0', 'bitmap writes match heap');
# Snapshot-correct scans after another connection publishes newer roots.
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; '.$force.'SELECT count(*) FROM p');
my $before = $reader->query_safe('SELECT count(*) FROM p WHERE id IN (100,1000)');
sql('DELETE FROM p WHERE id=100');
is($reader->query_safe('SELECT count(*) FROM p WHERE id IN (100,1000)'), $before, 'bitmap reads retain snapshot root');
$reader->query_safe('ROLLBACK');
$reader->quit;
# Historical and branch-bound index handles must open the matching table view.
my $old = sql(q{SELECT dolt_commit('-Am','bitmap fixture')});
sql(q{SELECT dolt_branch('bitmap_branch'); INSERT INTO p VALUES (20000,1,'main');});
is(sql($force."SELECT count(*) FROM p AS OF '$old' WHERE id IN (1000,20000)"), '1', 'historical bitmap uses historical table');
is(sql($force.q{SELECT count(*) FROM p AS OF 'bitmap_branch' WHERE id IN (1000,20000)}), '1', 'branch bitmap uses branch table');
is(sql($force.q{SELECT count(*) FROM p WHERE id IN (1000,20000)}), '2', 'current bitmap uses current table');
like(sql($force.q{EXPLAIN (COSTS OFF) SELECT * FROM p AS OF 'bitmap_branch' WHERE id IN (1000,20000)}), qr/Bitmap Index Scan/, 'revision query actually uses bitmap path');
# Arrays on multiple index columns expand into all key combinations. Descending
# keys, overlapping values, and NULLs must not change multiplicity or bounds.
sql(q{CREATE TABLE multi(a int,b int,v text) USING dolt_prolly_table;
 INSERT INTO multi SELECT g%10,g%17,g::text FROM generate_series(1,1000) g;
 CREATE INDEX multi_ab ON multi(a,b DESC) INCLUDE(v);});
my $multi = 'SELECT a,b,v FROM multi WHERE a IN (1,1,2,NULL) AND b IN (3,5,5,NULL) ORDER BY a,b,v';
is(sql($force.$multi), sql('SET enable_bitmapscan=off; SET enable_indexscan=off; SET enable_indexonlyscan=off; '.$multi), 'multicolumn array bitmap matches sequential scan');
like(sql($force.'EXPLAIN (COSTS OFF) '.$multi), qr/Bitmap Index Scan/, 'multicolumn arrays use bitmap access');
# More logical pages than a 64kB bitmap can retain exactly.
sql(q{CREATE TABLE lossy(id int) USING dolt_prolly_table;
 INSERT INTO lossy SELECT g FROM generate_series(1,400000) g;
 CREATE INDEX lossy_id ON lossy(id);});
my $lossy = 'SELECT count(*),sum(id) FROM lossy WHERE id BETWEEN 1 AND 370001 OR id BETWEEN 370009 AND 399999';
my $plan = sql($force."SET work_mem='64kB'; EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF) $lossy");
like($plan, qr/lossy=[1-9]/, 'memory-limited bitmap uses lossy pages');
is(sql($force."SET work_mem='64kB'; $lossy"), sql("SET enable_bitmapscan=off; SET enable_indexscan=off; SET enable_indexonlyscan=off; $lossy"), 'lossy pages recheck predicates');
$node->stop;
done_testing();
