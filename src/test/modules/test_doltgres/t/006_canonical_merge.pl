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
$node->stop;done_testing();
