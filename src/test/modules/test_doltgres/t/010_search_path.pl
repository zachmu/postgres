use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('search_path');
$node->init(extra=>['--encoding=UTF8','--locale=C']);$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
my $temp=$node->background_psql('postgres');
$temp->query_safe(q{CREATE TEMP TABLE t(v int) USING heap; INSERT INTO t VALUES(90)});
sql(q{SET default_table_access_method=dolt_prolly_table; CREATE ROLE reader; CREATE ROLE noperms; CREATE SCHEMA a; CREATE SCHEMA b; CREATE SCHEMA "Mixed"; CREATE SCHEMA "with,comma"; CREATE TABLE "with,comma".t(v int); INSERT INTO "with,comma".t VALUES(5); CREATE TABLE a."Case"(v int); INSERT INTO a."Case" VALUES(6); CREATE SCHEMA reader AUTHORIZATION reader; CREATE TABLE a.t(v int); CREATE TABLE b.t(v int); CREATE TABLE "Mixed".t(v int); CREATE TABLE reader.t(v int); INSERT INTO a.t VALUES(1); INSERT INTO b.t VALUES(2); INSERT INTO "Mixed".t VALUES(3); INSERT INTO reader.t VALUES(4); CREATE TABLE a.pg_class(marker int); INSERT INTO a.pg_class VALUES(77); CREATE TABLE a.pg_tables(marker int); INSERT INTO a.pg_tables VALUES(88); SELECT dolt_commit('-Am','base'); SELECT dolt_checkout('-b','other')});
for my $case ([q{a,b},'1'],[q{b,a},'2'],[q{missing,b,a},'2'],[q{"Mixed",a},'3'],[q{a,a,b},'1'],[q{"with,comma",a},'5']) {
 is(sql('SET search_path='.$case->[0].'; SELECT v FROM t'),$case->[1],'path '.$case->[0]);
}
is(sql(q{SET search_path=a,pg_catalog; SELECT marker FROM pg_class}),'77','explicit catalog ordering permits user shadow');
is(sql(q{SET search_path=a,pg_catalog; SELECT marker FROM pg_tables}),'88','user table can shadow bootstrap view');
is(sql(q{SET search_path=a; SELECT count(*)>0 FROM pg_class WHERE relname='pg_class'}),'t','implicit catalog precedes user schema');
is(sql(q{SET search_path=information_schema; SELECT column_name FROM columns WHERE table_schema='a' AND table_name='t'}),'v','information schema through search path');
# TODO: schema and table privileges during name resolution, once the ownership
# and ACL design decides what versioned storage records about roles.
is(sql(q{SET search_path=a,pg_catalog; SELECT c.oid='pg_class'::regclass FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON c.relnamespace=n.oid WHERE c.relname='pg_class' AND n.nspname='a'}),'t','regclass follows the same ordered path');
is(sql(q{SET search_path=a,b; SELECT pg_table_is_visible('a.t'::regclass),pg_table_is_visible('b.t'::regclass)}),'t|f','native visibility agrees with canonical shadowing');
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}
fails(q{SET search_path=''; SELECT * FROM t},qr/does not exist/,'empty path excludes user relations');
is(sql(q{SET search_path=''; SELECT v FROM b.t}),'2','explicit schema works with empty path');
is(sql(q{SET search_path=missing; SELECT to_regclass('t') IS NULL}),'t','missing-ok lookup preserves absence');
is(sql(q{SET search_path=a,pg_catalog; SELECT pg_table_is_visible('pg_catalog.pg_class'::regclass)}),'f','shadowed bootstrap relation is not visible');
is(sql(q{SET search_path=a; SELECT pg_table_is_visible('pg_catalog.pg_class'::regclass)}),'t','implicit bootstrap relation is visible');
my $s=$node->background_psql('postgres');
$s->query_safe(q{SET search_path=a,b; PREPARE p AS SELECT v FROM t});
is($s->query_safe('EXECUTE p'),'1','prepared initial path');
$s->query_safe(q{SET search_path=b,a});
is($s->query_safe('EXECUTE p'),'2','prepared query rebinds after path change');
$s->query_safe(q{BEGIN; SET LOCAL search_path=a,b});
is($s->query_safe('EXECUTE p'),'1','transaction-local path');
$s->query_safe('ROLLBACK');
is($s->query_safe('EXECUTE p'),'2','path restored after rollback');
$s->query_safe(q{SET search_path="$user",b; PREPARE rolepath AS SELECT v FROM t});
is($s->query_safe('EXECUTE rolepath'),'2','missing user schema skipped');
$s->query_safe(q{SELECT dolt_checkout('other'); SET search_path=b,a; UPDATE t SET v=20});
is($s->query_safe('SELECT v FROM b.t'),'20','write target uses ordered path');
is(sql(q{SET search_path=b,a; SELECT v FROM t}),'2','other branch write does not change main');
is($s->query_safe(q{SELECT v FROM t AS OF 'HEAD'}),'2','historical relation uses ordered path');
is($s->query_safe(q{SELECT v FROM a."Case"}),'6','quoted relation preserves case');
is($s->query_safe(q{SELECT to_regclass('a.case') IS NULL}),'t','unquoted relation does not match quoted case');
is($s->query_safe(q{COPY t TO STDOUT}),'20','COPY uses the selected schema');
$s->query_safe(q{BEGIN; INSERT INTO t VALUES(30); DELETE FROM t WHERE v=20});
is($s->query_safe('SELECT v FROM b.t'),'30','INSERT and DELETE resolve the same branch target');
$s->query_safe('ROLLBACK');
is($s->query_safe('SELECT v FROM b.t'),'20','write rollback preserves selected relation');
$s->query_safe(q{BEGIN; LOCK TABLE t IN ACCESS SHARE MODE; COMMIT});
is($s->query_safe('SELECT v FROM b.t'),'20','LOCK resolves the canonical relation');
$s->quit;
is($temp->query_safe(q{SET search_path=a,b; SELECT v FROM t}),'90','implicit session temporary namespace has priority');
is($temp->query_safe(q{SET search_path=a,pg_temp; SELECT v FROM t}),'1','explicit temporary namespace obeys path position');
is($temp->query_safe(q{SELECT v FROM pg_temp.t}),'90','explicit temporary alias resolves');
$temp->quit;$node->stop;done_testing();
