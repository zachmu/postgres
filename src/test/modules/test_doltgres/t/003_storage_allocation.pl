# Native resource allocation from canonical descriptors; no schema projection.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('storage_allocation');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sql(q{CREATE FUNCTION pg_catalog.test_storage_allocate(oid,text) RETURNS oid AS 'storage_resource','test_storage_allocate' LANGUAGE C STRICT});
# Retain an incompatible definition before activating branch views. Its physical
# table is dropped; later the fixture binds it to the same schema-free resource.
sql(q{CREATE TABLE t(id int PRIMARY KEY,v int) USING dolt_prolly_table; INSERT INTO t VALUES(1,7)});
my $legacy=sql(q{SELECT dolt_commit('-Am','integer schema')});
my $legacy_schema=sql(q{SELECT encode(bindschema,'hex') FROM pg_dolt_binding WHERE bindbranch='main' AND bindname='public.t'});
sql(q{DROP TABLE t});
sql(q{CREATE TABLE t(id int PRIMARY KEY,v text) USING dolt_prolly_table; INSERT INTO t VALUES(1,'main')});
sql(q{SELECT dolt_commit('-Am','base'); SELECT dolt_branch('other'); SELECT dolt_checkout('other')});
sql("SELECT dolt_branch('legacy','$legacy')");
my $allocate=q{SELECT test_storage_allocate(c.oid,'other') FROM pg_catalog.pg_class AS OF 'HEAD' c JOIN pg_dolt_binding b ON b.bindname='public.'||c.relname AND b.bindbranch='other' WHERE c.relname='t'};
my $pending=$node->background_psql('postgres');
$pending->query_safe('BEGIN');
my $aborted=$pending->query_safe($allocate);
my $path=$pending->query_safe("SELECT 'base/'||storagedb||'/'||storagefile FROM pg_dolt_storage WHERE storageid=$aborted");
ok(-f $node->data_dir.'/'.$path,'allocation creates a real relation file before commit');
is(sql("SELECT count(*) FROM pg_dolt_storage WHERE storageid=$aborted"),'0','uncommitted resource is invisible to other sessions');
$pending->query_safe("UPDATE pg_dolt_binding SET bindrelid=$aborted WHERE bindbranch='other' AND bindname='public.t'");
is($pending->query_safe(q{SELECT count(*) FROM "postgres/other".public.t}),'0','allocating transaction sees its own provisional binding');
is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'1','concurrent session cannot see provisional resource publication');
$pending->query_safe('ROLLBACK');
is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'1','abort restores the prior binding along with storage cleanup');
sql('CHECKPOINT');
ok(!-e $node->data_dir.'/'.$path,'abort removes allocated file through native pending-delete machinery');
is(sql("SELECT count(*) FROM pg_dolt_storage WHERE storageid=$aborted"),'0','abort also removes resource metadata');
$pending->query_safe('BEGIN; SAVEPOINT s');
my $rolled=$pending->query_safe($allocate);
$path=$pending->query_safe("SELECT 'base/'||storagedb||'/'||storagefile FROM pg_dolt_storage WHERE storageid=$rolled");
$pending->query_safe('ROLLBACK TO s; COMMIT');
sql('CHECKPOINT');
ok(!-e $node->data_dir.'/'.$path,'savepoint rollback removes file');
is(sql("SELECT count(*) FROM pg_dolt_storage WHERE storageid=$rolled"),'0','savepoint rollback removes resource row');
$pending->quit;
# Fixture publishes a binding to the existing canonical definition. Full SQL
# CREATE must publish a new definition and membership as well in one transaction.
my $plan=$node->background_psql('postgres');
$plan->query_safe(q{PREPARE current_binding AS SELECT count(*) FROM "postgres/other".public.t; EXECUTE current_binding});
my $publisher=$node->background_psql('postgres');
$publisher->query_safe('BEGIN');
my $id=$publisher->query_safe($allocate);
$publisher->query_safe("UPDATE pg_dolt_binding SET bindrelid=$id WHERE bindbranch='other' AND bindname='public.t'");
is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'1','publication remains invisible until its SQL transaction commits');
$publisher->query_safe('COMMIT');$publisher->quit;
is(sql("SELECT count(*) FROM pg_class WHERE oid=$id"),'0','committed allocation has no physical pg_class row');
is(sql("SELECT count(*) FROM pg_attribute WHERE attrelid=$id"),'0','committed allocation has no physical column definitions');
is(sql("SELECT count(*) FROM pg_type WHERE typrelid=$id"),'0','committed allocation has no persisted runtime row type');
is(sql("SELECT storageimported FROM pg_dolt_storage WHERE storageid=$id"),'f','resource owns its file independently of native schema catalogs');

is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'0','normal qualified SQL reads initialized empty resource with canonical schema');
is($plan->query_safe('EXECUTE current_binding'),'0','read-committed prepared plan rebinds after resource publication');
$plan->quit;
is(sql(q{SELECT * FROM "postgres/main".public.t}),'1|main','other branch retains its original resource and rows');
is(sql(q{SELECT * FROM t AS OF 'HEAD'}),'1|main','retained query still reads canonical commit content');
# Internal publication fixture, not SQL CREATE/ALTER: the old branch falls back
# to its retained contents; the new branch has an initialized empty local root.
sql("BEGIN; DELETE FROM pg_dolt_binding WHERE bindbranch='legacy' AND bindname='public.t'; INSERT INTO pg_dolt_binding(bindrelid,bindbranch,bindname,bindschema) VALUES($id,'legacy','public.t',decode('$legacy_schema','hex')); COMMIT");
is(sql(q{SELECT count(DISTINCT bindrelid),count(DISTINCT bindschema) FROM pg_dolt_binding WHERE bindname='public.t' AND bindbranch IN ('legacy','other')}),'1|2','incompatible canonical definitions share one local resource identifier');
my $index=sql(q{SELECT test_storage_allocate(c.oid,'other') FROM pg_catalog.pg_class AS OF 'HEAD' c JOIN pg_dolt_binding b ON b.bindname='public.'||c.relname AND b.bindbranch='other' WHERE c.relname='t_pkey'});
is(sql("SELECT storagekind FROM pg_dolt_storage WHERE storageid=$index"),'i','native index definition can allocate an independent index file');
is(sql("SELECT count(*) FROM pg_class WHERE oid=$index"),'0','index allocation also has no physical schema row');
# The other branch's primary key index is bound to an empty resource too, as
# its table is: an index scan returns the rows its entries hold.
sql("UPDATE pg_dolt_binding SET bindrelid=$index WHERE bindbranch='other' AND bindname='public.t_pkey'");
is(sql(q{SELECT row_to_json(a),b.v FROM "postgres/legacy".public.t a LEFT JOIN "postgres/other".public.t b USING(id)}),'{"id":1,"v":7}|','one query binds incompatible definitions to one resource without mixing roots');
for my $case (['legacy','id:integer,v:integer'],['other','id:integer,v:text'])
{
 my ($branch,$expected)=@$case;
 is(sql("SELECT string_agg(a.attname||':'||format_type(a.atttypid,a.atttypmod),',' ORDER BY a.attnum) FROM \"postgres/$branch\".pg_catalog.pg_class c JOIN \"postgres/$branch\".pg_catalog.pg_attribute a ON a.attrelid=c.oid WHERE c.relname='t' AND a.attnum>0"),$expected,'catalog columns follow branch schema rather than shared storage identity');
}
$node->stop('immediate');$node->start;
is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'0','WAL recovery retains independently allocated file and empty root');
is(sql(q{SELECT v FROM "postgres/legacy".public.t}),'7','restart preserves incompatible binding to shared resource');
$node->backup('allocated');
my $standby=PostgreSQL::Test::Cluster->new('allocated_standby');$standby->init_from_backup($node,'allocated',has_streaming=>1);$standby->start;
is($standby->safe_psql('postgres',q{SELECT count(*) FROM "postgres/other".public.t}),'0','standby reads allocation without any persisted schema copy');
$standby->stop;
# Crash before commit must not publish resource metadata or replace a binding.
my $crash=$node->background_psql('postgres');$crash->query_safe('BEGIN');
my $crashed=$crash->query_safe($allocate);
$path=$crash->query_safe("SELECT 'base/'||storagedb||'/'||storagefile FROM pg_dolt_storage WHERE storageid=$crashed");
$node->stop('immediate');$crash->quit;$node->start;
is(sql("SELECT count(*) FROM pg_dolt_storage WHERE storageid=$crashed"),'0','crash leaves no committed resource row');
diag('uncommitted resource file after crash: '.(-e $node->data_dir.'/'.$path ? 'present' : 'absent'));
# PostgreSQL crash recovery may leave an unreachable file from an uncommitted
# CREATE; reclamation is a separate resource-lifecycle requirement.
is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'0','aborted allocation cannot replace committed binding after crash');
$node->stop;done_testing();
