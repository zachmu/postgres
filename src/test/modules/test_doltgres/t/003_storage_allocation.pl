# Native resource allocation from canonical descriptors; no schema projection.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('storage_allocation');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sql(q{CREATE FUNCTION pg_catalog.test_storage_allocate(oid,text) RETURNS oid AS 'storage_resource','test_storage_allocate' LANGUAGE C STRICT});
sql(q{CREATE TABLE t(id int PRIMARY KEY,v text) USING dolt_prolly_table; INSERT INTO t VALUES(1,'main')});
sql(q{SELECT dolt_commit('-Am','base'); SELECT dolt_branch('other'); SELECT dolt_checkout('other')});
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
my $old=$node->background_psql('postgres');
$old->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; PREPARE retained_binding AS SELECT count(*) FROM "postgres/other".public.t; EXECUTE retained_binding});
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
is($old->query_safe('EXECUTE retained_binding'),'1','old snapshot and prepared binding retain the prior resource');
is($plan->query_safe('EXECUTE current_binding'),'0','read-committed prepared plan rebinds after resource publication');
$old->query_safe('ROLLBACK');$old->quit;$plan->quit;
is(sql(q{SELECT * FROM "postgres/main".public.t}),'1|main','other branch retains its original resource and rows');
is(sql(q{SELECT * FROM t AS OF 'HEAD'}),'1|main','retained query still reads canonical commit content');
my $index=sql(q{SELECT test_storage_allocate(c.oid,'other') FROM pg_catalog.pg_class AS OF 'HEAD' c JOIN pg_dolt_binding b ON b.bindname='public.'||c.relname AND b.bindbranch='other' WHERE c.relname='t_pkey'});
is(sql("SELECT storagekind FROM pg_dolt_storage WHERE storageid=$index"),'i','native index definition can allocate an independent index file');
is(sql("SELECT count(*) FROM pg_class WHERE oid=$index"),'0','index allocation also has no physical schema row');
$node->stop('immediate');$node->start;
is(sql(q{SELECT count(*) FROM "postgres/other".public.t}),'0','WAL recovery retains independently allocated file and empty root');
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
