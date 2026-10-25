use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('lifecycle');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->append_conf('postgresql.conf',"dolt_catalog_cache_entries=2\nmax_parallel_workers_per_gather=2\n");
$node->start;
like($node->safe_psql('postgres',q{SET debug_parallel_query=on; EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF) SELECT sum(i) FROM generate_series(1,100) i}),qr/Gather/,'ordinary native query still executes through a parallel plan');
my $s=$node->background_psql('postgres',on_error_stop=>0);
sub sql {return $s->query_safe($_[0]);}
sql(q{SET client_min_messages=warning; CREATE TABLE t(id int PRIMARY KEY,x int) USING dolt_prolly_table;
INSERT INTO t VALUES(1,10),(2,20);
CREATE TABLE _t(id int) USING dolt_prolly_table;
CREATE FUNCTION f(t) RETURNS int LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$SELECT ($1).x$$;
SELECT dolt_commit('-Am','lifetime base');});
my $base=sql(q{SELECT commit_hash FROM dolt_log('-n','1')});
sql(q{SELECT dolt_checkout('main')});
sql(qq{PREPARE oldrows AS SELECT f(t) FROM t AS OF '$base' ORDER BY id});
sql(qq{PREPARE oldarray AS SELECT cardinality(array_agg(t)) FROM t AS OF '$base'});
my $oldoid=sql(qq{SELECT oid FROM pg_type AS OF '$base' WHERE typname='t'});
my @revisions=($base);
for my $i (1..8) {
 sql("ALTER TABLE t ALTER COLUMN x SET STATISTICS ".(100+$i));
 sql("SELECT dolt_commit('-Am','revision $i')");
 push @revisions,sql(q{SELECT commit_hash FROM dolt_log('-n','1')});
}
# Count in Perl: a function call would resolve through, and reload, the
# session's branch view, measuring the measurement's own residency.
my $resident=q{SELECT used_bytes FROM pg_backend_memory_contexts WHERE name='catalog view rows' AND used_bytes>16384};
sub resident {my $rows=sql($resident);return scalar(grep {length} split(/\n/,$rows));}
cmp_ok(resident(),'<=' ,2,'decoded catalog row residency respects cache target');
is(sql('EXECUTE oldrows'),"10\n20",'prepared routine and composite handles survive eviction');
is(sql('EXECUTE oldarray'),'2','generated array identity survives collisions and eviction');
is(sql(qq{SELECT oid FROM pg_type AS OF '$base' WHERE typname='t'}),$oldoid,'reload preserves original runtime type identifier');
for my $i (1..8) {
 is(sql("SELECT attstattarget FROM pg_attribute AS OF '$revisions[$i]' WHERE attname='x'"),100+$i,"cold revision $i reloads its own metadata");
}
cmp_ok(resident(),'<=' ,2,'repeated reloads reclaim old decoded rows');
sql(q{SET statement_timeout='15s'; SET debug_parallel_query=on; SET parallel_setup_cost=0; SET parallel_tuple_cost=0; SET min_parallel_table_scan_size=0});
unlike(sql(qq{EXPLAIN (COSTS OFF) SELECT f(t) FROM t}),qr/Gather/,'canonical function and row context stay in leader');
unlike(sql(qq{EXPLAIN (COSTS OFF) SELECT oid FROM pg_type AS OF '$base' WHERE typname='t'}),qr/Gather/,'revision-qualified catalogs stay in leader');
is(sql('EXECUTE oldrows'),"10\n20",'forced parallel setting cannot ship contextual handles');
is(sql("SELECT format_type($oldoid,NULL)"),'t','OID-producing casts stay in leader before constant folding');
sql(q{BEGIN READ ONLY});
is(sql(qq{SELECT sum(x) FROM t AS OF '$base'}),'30','cold retained rows work in read-only transaction');
is(sql('SELECT txid_current_if_assigned() IS NULL'),'t','read-only catalog reload assigns no transaction ID');
sql('COMMIT');
sql(q{BEGIN; DECLARE held CURSOR WITH HOLD FOR SELECT attstattarget FROM pg_attribute WHERE attname='x'; COMMIT});
sql('ALTER TABLE t ALTER COLUMN x SET STATISTICS 999');
is(sql('FETCH ALL FROM held'),'108','held catalog cursor preserves its old definition');
sql('CLOSE held');
# A branch head's row type keeps one durable OID across schema versions; an
# aborted change leaves that OID naming the surviving definition.
my $durable=sql(q{SELECT 't'::regtype::oid});
sql('BEGIN; ALTER TABLE t ADD COLUMN ghost int');
is(sql(q{SELECT 't'::regtype::oid}),$durable,'schema change keeps the row type OID');
sql('ROLLBACK');
is(sql(q{SELECT count(*) FROM pg_attribute WHERE attrelid=(SELECT typrelid FROM pg_type WHERE oid=}.$durable.q{) AND attname='ghost'}),'0','an aborted schema change leaves the durable OID naming the old definition');
sql('BEGIN; ALTER TABLE t ALTER COLUMN x SET STATISTICS 1000');
sql('SAVEPOINT child; ALTER TABLE t ADD COLUMN ghost int');
is(sql(q{SELECT count(*) FROM pg_attribute WHERE attrelid='t'::regclass AND attname='ghost'}),'1','child schema change is visible inside the child');
sql('ROLLBACK TO child');
is(sql(q{SELECT count(*) FROM pg_attribute WHERE attrelid='t'::regclass AND attname='ghost'}),'0','savepoint rollback restores the parent schema');
sql('ROLLBACK TO child');
is(sql("SELECT format_type($durable,NULL)"),'t','parent schema remains usable after child abort');
sql('ROLLBACK');
sql(q{SELECT dolt_checkout('-b','other'); ALTER TABLE t ALTER COLUMN x SET STATISTICS 222; UPDATE t SET x=x+1; SELECT dolt_commit('-Am','other metadata')});
$s->quit;
$node->backup('lifetime');
my $standby=PostgreSQL::Test::Cluster->new('lifecycle_standby');
$standby->init_from_backup($node,'lifetime',has_streaming=>1);$standby->start;
is($standby->safe_psql('postgres',q{SELECT sum(x) FROM t AS OF 'other'}),'32','standby reads branch data without publishing bindings');
is($standby->safe_psql('postgres',q{SELECT attstattarget FROM pg_attribute AS OF 'other' WHERE attname='x'}),'222','standby reconstructs branch catalog values');
is($standby->safe_psql('postgres',qq{SELECT sum(x) FROM t AS OF '$base'}),'30','standby reads retained schema and data together');
$standby->stop;
my $pending=$node->background_psql('postgres');
$pending->query_safe(q{BEGIN; ALTER TABLE t ALTER COLUMN x SET STATISTICS 777; UPDATE t SET x=777});
$node->stop('immediate');$pending->quit;$node->start;
is($node->safe_psql('postgres',q{SELECT attstattarget FROM pg_attribute WHERE attrelid='t'::regclass AND attname='x'}),'999','crash discards uncommitted catalog publication');
is($node->safe_psql('postgres',q{SELECT sum(x) FROM t}),'30','crash discards matching uncommitted data');
is($node->safe_psql('postgres',q{SELECT attstattarget FROM pg_attribute AS OF 'other' WHERE attname='x'}),'222','committed other-branch catalog survives crash');
$node->stop;done_testing();
