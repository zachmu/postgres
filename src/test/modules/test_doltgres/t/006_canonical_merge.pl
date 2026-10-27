# Merge must enumerate canonical membership, including schema-less resources.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('canonical_merge');
$node->init(extra=>['--encoding=UTF8','--locale=C']);$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sql(q{CREATE TABLE seed(id int) USING dolt_prolly_table; SELECT dolt_commit('-Am','base'); SELECT dolt_checkout('-b','activation'); SELECT dolt_checkout('main')});
sql(q{CREATE TABLE t(id int PRIMARY KEY,v int UNIQUE); INSERT INTO t VALUES(1,10),(2,20); SELECT dolt_commit('-Am','canonical table'); SELECT dolt_branch('side')});
sql(q{UPDATE t SET v=11 WHERE id=1; SELECT dolt_commit('-Am','main change')});
sql(q{SELECT dolt_checkout('side'); UPDATE t SET v=22 WHERE id=2; SELECT dolt_commit('-Am','side change')});
is(sql(q{SELECT conflicts FROM dolt_merge('side')}),'0','canonical tables merge without conflicts');
is(sql(q{SELECT * FROM t ORDER BY id}),"1|11\n2|22",'three-way merge retains both canonical-only table edits');
sql(q{SELECT dolt_branch('conflict'); UPDATE t SET v=12 WHERE id=1; SELECT dolt_commit('-Am','ours')});
sql(q{SELECT dolt_checkout('conflict'); UPDATE t SET v=13 WHERE id=1; SELECT dolt_commit('-Am','theirs')});
is(sql(q{SELECT conflicts FROM dolt_merge('conflict')}),'1','canonical row conflict returns status without SQL error');
sql(q{SELECT dolt_conflicts_resolve('--theirs','t'); SELECT dolt_commit('-Am','resolved')});
is(sql(q{SELECT * FROM t ORDER BY id}),"1|13\n2|22",'conflict resolution writes canonical resource');
$node->stop('immediate');$node->start;
is(sql(q{SELECT * FROM t ORDER BY id}),"1|13\n2|22",'merged canonical data survives recovery');
# ---- schema objects merge object by object ----
sql(q{CREATE FUNCTION kept() RETURNS int LANGUAGE sql AS $$SELECT 1$$; CREATE FUNCTION dropped() RETURNS int LANGUAGE sql AS $$SELECT 2$$; SELECT dolt_commit('-Am','schema base'); SELECT dolt_branch('schema')});
sql(q{SELECT dolt_checkout('schema');
CREATE FUNCTION theirs() RETURNS int LANGUAGE sql AS $$SELECT 3$$;
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.v := NEW.v * 100; RETURN NEW; END$$;
CREATE TRIGGER t_stamp BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION stamp();
CREATE TABLE added(id int PRIMARY KEY, w text); CREATE INDEX added_w ON added(w); INSERT INTO added VALUES (1,'their row');
DROP FUNCTION dropped();
SELECT dolt_commit('-Am','their schema')});
sql(q{CREATE FUNCTION ours() RETURNS int LANGUAGE sql AS $$SELECT 4$$; INSERT INTO seed VALUES (7); SELECT dolt_commit('-Am','our schema')});
is(sql(q{SELECT fast_forward, conflicts FROM dolt_merge('schema')}),'0|0','branches with different schema objects merge');
is(sql(q{SELECT kept()+theirs()+ours()}),'8','routines from both branches, and unchanged ones, are merged');
is(sql(q{SELECT count(*) FROM pg_proc WHERE proname='dropped'}),'0','a routine dropped on one branch is dropped');
is(sql(q{SELECT w FROM added WHERE w='their row'}),'their row','a table added on one branch arrives with its rows and index');
sql(q{INSERT INTO t VALUES (3,3)});
is(sql(q{SELECT v FROM t WHERE id=3}),'300','a trigger added on one branch fires after merge');
is(sql(q{SELECT count(*) FROM seed WHERE id=7}),'1','our rows survive a schema merge');
is(sql(q{SELECT count(*) FROM dolt_log WHERE message='their schema'}),'1','the merge commit has their history');

sql(q{SELECT dolt_commit('-Am','after merge'); SELECT dolt_checkout('-b','ahead'); CREATE FUNCTION later() RETURNS int LANGUAGE sql AS $$SELECT 5$$; SELECT dolt_commit('-Am','ahead'); SELECT dolt_checkout('main')});
is(sql(q{SELECT fast_forward FROM dolt_merge('ahead')}),'1','a fast-forward brings new schema objects');
is(sql(q{SELECT later()}),'5','fast-forwarded routine is callable');

sql(q{SELECT dolt_checkout('-b','pick'); CREATE FUNCTION picked() RETURNS int LANGUAGE sql AS $$SELECT 6$$; SELECT dolt_commit('-Am','pick me'); SELECT dolt_checkout('main')});
my $pick=sql(q{SELECT commit_hash FROM dolt_log('pick') WHERE message='pick me'});
sql(qq{SELECT dolt_cherry_pick('$pick')});
is(sql(q{SELECT picked()}),'6','cherry-pick applies a schema change');

sql(q{SELECT dolt_branch('both'); CREATE OR REPLACE FUNCTION kept() RETURNS int LANGUAGE sql AS $$SELECT 10$$; SELECT dolt_commit('-Am','our kept')});
sql(q{SELECT dolt_checkout('both'); CREATE OR REPLACE FUNCTION kept() RETURNS int LANGUAGE sql AS $$SELECT 20$$; SELECT dolt_commit('-Am','their kept'); SELECT dolt_checkout('main')});
is(sql(q{SELECT conflicts FROM dolt_merge('both')}),'1','a routine changed differently on both branches is a schema conflict');
is(sql(q{SELECT table_name||': '||description FROM dolt_schema_conflicts}),'public.kept(): both branches changed routine public.kept()','the schema conflict is recorded');
is(sql(q{SELECT kept()}),'10','a schema conflict keeps our side');
sql(q{SELECT dolt_conflicts_resolve('--theirs','public.kept()'); SELECT dolt_commit('-Am','took their kept')});
is(sql(q{SELECT kept()}),'20','a schema conflict resolves to their side');

sql(q{SELECT dolt_branch('layout'); ALTER TABLE added ADD COLUMN z int; SELECT dolt_commit('-Am','our layout')});
sql(q{SELECT dolt_checkout('layout'); INSERT INTO added VALUES (2,'rows'); SELECT dolt_commit('-Am','their rows'); SELECT dolt_checkout('main')});
is(sql(q{SELECT conflicts FROM dolt_merge('layout')}),'0','rows merge across a table definition change');
is(sql(q{SELECT string_agg(id||':'||w||':'||coalesce(z::text,'-'), ',' ORDER BY id) FROM added}),'1:their row:-,2:rows:-','their rows take our added column');
$node->stop;done_testing();
