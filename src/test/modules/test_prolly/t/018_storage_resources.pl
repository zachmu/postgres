# Stable local storage identity is separate from canonical definitions.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('storage_resources');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);
$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sub fails { my($q,$pattern,$label)=@_;my($s,$o,$e)=$node->psql('postgres',$q);isnt($s,0,$label);like($e,$pattern,"$label diagnostic"); }
sql(q{CREATE ROLE reader; CREATE TABLE t(id int PRIMARY KEY,v text) USING dolt_prolly_table; INSERT INTO t VALUES(1,'base'); GRANT SELECT ON t TO reader});
my $head=sql(q{SELECT dolt_commit('-Am','base')});
my $resource=sql(q{SELECT bindrelid FROM pg_dolt_binding WHERE bindbranch='main' AND bindname='public.t'});
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),'2','capture registers table and index resources');
is(sql(q{SELECT bool_and(s.storageid=c.oid AND s.storagefile=pg_relation_filenode(c.oid) AND s.storageam=c.relam AND s.storagekind=c.relkind AND s.storageschema=b.bindschema) FROM pg_dolt_storage s JOIN pg_class c ON c.oid=s.storageid JOIN pg_dolt_binding b ON b.bindrelid=s.storageid}),'t','imported resource records describe files and exact canonical schemas');
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING')}),'0','resource registration does not change canonical content');
fails(q{SET ROLE reader; SELECT * FROM pg_dolt_storage},qr/permission denied/,'resource records are internal');
sql(q{SELECT dolt_branch('other'); SELECT dolt_checkout('other'); UPDATE t SET v='other'});
is(sql(q{SELECT a.v,b.v FROM "postgres/main".public.t a JOIN "postgres/other".public.t b USING(id)}),'base|other','one resource supplies independently selected branch roots');
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),'2','branch creation shares resources without schema copies');
is(sql(q{SET ROLE reader; BEGIN READ ONLY; SELECT row_to_json(t) FROM "postgres/other".public.t t; SELECT pg_current_xact_id_if_assigned() IS NULL; COMMIT}),"{\"id\":1,\"v\":\"other\"}\nt",'read-only canonical descriptor opens a stable resource without writes');
my $locked=$node->background_psql('postgres');
$locked->query_safe(q{BEGIN; SELECT * FROM "postgres/other".public.t});
fails(q{LOCK TABLE t IN ACCESS EXCLUSIVE MODE NOWAIT},qr/LOCK TABLE can only/,'lock test requires a transaction');
fails(q{BEGIN; LOCK TABLE t IN ACCESS EXCLUSIVE MODE NOWAIT},qr/could not obtain lock/,'bound scan locks the shared physical resource');
is($locked->query_safe(q{SELECT count(*) FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='relation' AND relation>=3221225472::oid}),'0','backend-local canonical handles are not shared lock identities');
$locked->query_safe('ROLLBACK');$locked->quit;
# A stale plan must not silently adopt a resource with a different schema.
sql(q{PREPARE held AS SELECT * FROM "postgres/other".public.t; EXECUTE held; DISCARD PLANS; EXECUTE held});
fails("BEGIN; UPDATE pg_dolt_storage SET storageschema=decode(repeat('00',32),'hex') WHERE storageid=$resource; SELECT * FROM \"postgres/other\".public.t",qr/runtime catalog binding unavailable/,'incompatible resource schema fails closed');
is(sql(q{SELECT * FROM "postgres/other".public.t}),'1|other','failed resource change rolls back');
# The statement snapshot applies to both mapping and resource rows.
my $old=$node->background_psql('postgres');
$old->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM "postgres/other".public.t});
my $saved=sql("SELECT row_to_json(s)::text FROM pg_dolt_storage s WHERE storageid=$resource");
sql("DELETE FROM pg_dolt_storage WHERE storageid=$resource");
is($old->query_safe(q{SELECT * FROM "postgres/other".public.t}),'1|other','old snapshot retains its resource after metadata removal');
fails(q{SELECT * FROM "postgres/other".public.t},qr/runtime catalog binding unavailable/,'new snapshot rejects a missing resource');
$old->query_safe('ROLLBACK');$old->quit;
$saved =~ s/'/''/g;
sql("INSERT INTO pg_dolt_storage SELECT * FROM jsonb_populate_record(NULL::pg_dolt_storage,'$saved')");
is(sql(q{SELECT * FROM "postgres/other".public.t}),'1|other','restored resource resumes reads without rebuilding schemas');
sql(q{SELECT dolt_checkout('other')});
my $before=sql(q{SELECT count(*) FROM pg_dolt_storage});
sql(q{BEGIN; CREATE TABLE aborted_resource(id int PRIMARY KEY); ROLLBACK});
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),$before,'aborted CREATE leaves no resource metadata');
sql(q{BEGIN; SAVEPOINT s; CREATE TABLE rolled_resource(id int PRIMARY KEY); ROLLBACK TO s; COMMIT});
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),$before,'savepoint rollback discards resource metadata');
$node->stop('immediate');$node->start;
is(sql(q{SELECT * FROM "postgres/other".public.t}),'1|other','resource and branch root recover together');
$node->backup('resources');
my $standby=PostgreSQL::Test::Cluster->new('resource_standby');
$standby->init_from_backup($node,'resources',has_streaming=>1);$standby->start;
is($standby->safe_psql('postgres',q{SET ROLE reader; SELECT row_to_json(t) FROM "postgres/other".public.t t; SELECT pg_current_xact_id_if_assigned() IS NULL}),"{\"id\":1,\"v\":\"other\"}\nt",'cold standby reads resource metadata and pages without a write');
$standby->stop;$node->stop;
done_testing();
