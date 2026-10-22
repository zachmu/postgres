# Native AM callbacks with explicit branch/schema/storage authority.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('storage_writes');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sub fails { my($q,$pattern,$label)=@_;my($s,$o,$e)=$node->psql('postgres',$q);isnt($s,0,$label);like($e,$pattern,"$label diagnostic"); }
sql(q{CREATE FUNCTION pg_catalog.test_storage_allocate(oid,text) RETURNS oid AS 'storage_resource','test_storage_allocate' LANGUAGE C STRICT;
CREATE FUNCTION pg_catalog.test_storage_write(text,text,text,text[],bigint) RETURNS bigint AS 'storage_resource','test_storage_write' LANGUAGE C STRICT;
CREATE ROLE writer; CREATE ROLE outsider;
CREATE TABLE t(id int PRIMARY KEY,v int UNIQUE) USING dolt_prolly_table; INSERT INTO t VALUES(1,7)});
my $legacy=sql(q{SELECT dolt_commit('-Am','integer definition')});
my $schema=sql(q{SELECT encode(bindschema,'hex') FROM pg_dolt_binding WHERE bindbranch='main' AND bindname='public.t'});
my %index_schemas;
for my $index ('t_pkey','t_v_key')
{
 $index_schemas{$index}=sql("SELECT encode(bindschema,'hex') FROM pg_dolt_binding WHERE bindbranch='main' AND bindname='public.$index'");
}
sql(q{DROP TABLE t; CREATE TABLE t(id int PRIMARY KEY,v text UNIQUE) USING dolt_prolly_table; INSERT INTO t VALUES(1,'main'); GRANT SELECT,INSERT ON t TO writer; SELECT dolt_commit('-Am','text definition'); SELECT dolt_branch('other')});
sql("SELECT dolt_branch('legacy','$legacy')");
my $resource=sql(q{SELECT test_storage_allocate(c.oid,'other') FROM pg_catalog.pg_class AS OF 'HEAD' c WHERE c.relname='t'});
sql("UPDATE pg_dolt_binding SET bindrelid=$resource WHERE bindbranch='other' AND bindname='public.t'; DELETE FROM pg_dolt_binding WHERE bindbranch='legacy' AND bindname='public.t'; INSERT INTO pg_dolt_binding VALUES($resource,'legacy','public.t',decode('$schema','hex'))");
for my $index ('t_pkey','t_v_key')
{
 my $id=sql("SELECT test_storage_allocate(c.oid,'other') FROM pg_catalog.pg_class AS OF 'HEAD' c WHERE c.relname='$index'");
 sql("BEGIN; UPDATE pg_dolt_binding SET bindrelid=$id WHERE bindbranch='other' AND bindname='public.$index'; DELETE FROM pg_dolt_binding WHERE bindbranch='legacy' AND bindname='public.$index'; INSERT INTO pg_dolt_binding VALUES($id,'legacy','public.$index',decode('$index_schemas{$index}','hex')); COMMIT");
}
my $resources=sql(q{SELECT count(*) FROM pg_dolt_storage});
is(sql(q{SELECT test_storage_write('legacy','public.t','insert',ARRAY['2','8'],0)}),'2','native insert imports old branch contents and allocates occurrence handle');
is(sql(q{SET ROLE writer; SELECT test_storage_write('other','public.t','insert',ARRAY['2','text'],0)}),'1','authorized non-owner writes through canonical definition');
is(sql(q{SELECT a.v,b.v FROM "postgres/legacy".public.t a JOIN "postgres/other".public.t b USING(id)}),'8|text','two incompatible writable schemas share a resource and preserve their roots');
is(sql(q{SELECT active_branch()}),'main','explicit write binding does not switch session branch');
is(sql(q{SELECT * FROM "postgres/main".public.t}),'1|main','writes leave another resource and branch untouched');
is(sql(q{SELECT test_storage_write('legacy','public.t','update',ARRAY['1','9'],1)}),'3','native update uses bound branch and replaces occurrence');
is(sql(q{SELECT test_storage_write('legacy','public.t','delete',ARRAY[]::text[],2)}),'0','native delete uses bound branch');
is(sql(q{SELECT * FROM "postgres/legacy".public.t}),'1|9','updated branch has correct rows');
is(sql(q{SELECT * FROM "postgres/other".public.t}),'2|text','other schema remains unchanged after update and delete');
fails(q{SELECT test_storage_write('other','public.t','insert',ARRAY['2','different'],0)},qr/duplicate key violates unique constraint "t_pkey"/,'bound primary index rejects duplicate key');
fails(q{SELECT test_storage_write('other','public.t','insert',ARRAY['3','text'],0)},qr/duplicate key violates unique constraint "t_v_key"/,'bound secondary unique index rejects duplicate value');
is(sql(q{SELECT * FROM "postgres/other".public.t}),'2|text','failed index updates roll back table root too');

fails(q{SET ROLE outsider; SELECT test_storage_write('other','public.t','insert',ARRAY['3','denied'],0)},qr/permission denied/,'canonical grants reject unauthorized write');
fails(q{SET ROLE writer; SELECT test_storage_write('other','public.t','delete',ARRAY[]::text[],1)},qr/permission denied/,'INSERT grant does not grant DELETE authority');
fails(q{BEGIN READ ONLY; SELECT test_storage_write('other','public.t','insert',ARRAY['3','denied'],0)},qr/read-only/,'read-only transaction rejects write before page changes');
fails(q{SELECT test_storage_write('other','public.t','insert',ARRAY[NULL,'invalid'],0)},qr/null value.*violates not-null/,'native executor checks canonical NOT NULL constraint');
sql(q{BEGIN; SELECT test_storage_write('other','public.t','insert',ARRAY['3','abort'],0); ROLLBACK});
is(sql(q{SELECT * FROM "postgres/other".public.t}),'2|text','abort hides appended roots');
sql(q{BEGIN; SAVEPOINT s; SELECT test_storage_write('other','public.t','insert',ARRAY['3','savepoint'],0); ROLLBACK TO s; COMMIT});
is(sql(q{SELECT * FROM "postgres/other".public.t}),'2|text','savepoint rollback hides appended roots');
my $held=$node->background_psql('postgres');
$held->query_safe(q{BEGIN; SELECT test_storage_write('legacy','public.t','insert',ARRAY['4','10'],0)});
is(sql(q{SELECT * FROM "postgres/legacy".public.t}),'1|9','concurrent readers cannot see uncommitted root');
is($held->query_safe(q{SELECT count(*) FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='relation' AND relation>=3221225472::oid}),'0','AM writer never locks a backend-local definition OID');
fails(q{SET lock_timeout='100ms'; UPDATE pg_dolt_binding SET bindschema=bindschema WHERE bindbranch='legacy' AND bindname='public.t'},qr/lock timeout/,'write pins binding against concurrent schema publication');
fails(q{SET lock_timeout='100ms'; UPDATE pg_dolt_ref SET reftarget=reftarget WHERE refname='working/legacy'},qr/lock timeout/,'write pins branch membership against concurrent DROP');
sql(q{SET lock_timeout='100ms'; UPDATE pg_dolt_ref SET reftarget=reftarget WHERE refname='working/other'});
pass('membership pin does not lock another branch ref');
$held->query_safe('ROLLBACK');$held->quit;
my $old=$node->background_psql('postgres');
$old->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM "postgres/legacy".public.t});
sql(q{SELECT test_storage_write('legacy','public.t','insert',ARRAY['5','11'],0)});
$old->query_safe('\\set ON_ERROR_STOP 0');
$old->query(q{SELECT test_storage_write('legacy','public.t','insert',ARRAY['6','12'],0)});
# background_psql reports SQL errors in its captured stderr.
like($old->{stderr},qr/prolly root advanced after snapshot/,'stale writer rejects a newer committed root');
$old->{stderr}='';
$old->query_safe('ROLLBACK');$old->quit;
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),$resources,'branch writes and revisions allocate no new storage resources');
$node->stop('immediate');$node->start;
is(sql(q{SELECT string_agg(id||':'||v,',' ORDER BY id) FROM "postgres/legacy".public.t}),'1:9,5:11','restart recovers committed integer-schema roots only');
is(sql(q{SELECT * FROM "postgres/other".public.t}),'2|text','restart independently recovers text-schema root');
$node->backup('written');
my $standby=PostgreSQL::Test::Cluster->new('writes_standby');$standby->init_from_backup($node,'written',has_streaming=>1);$standby->start;
is($standby->safe_psql('postgres',q{SELECT a.v,b.v FROM "postgres/legacy".public.t a CROSS JOIN "postgres/other".public.t b WHERE a.id=1}),'9|text','standby reads both schemas from shared resource after WAL replay');
$standby->stop;$node->stop;done_testing();
