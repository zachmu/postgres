# Independently versioned objects need no table to activate their catalogs.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('workspace_objects');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{BEGIN; CREATE FUNCTION aborted_fn() RETURNS int LANGUAGE sql AS 'SELECT 1'; ROLLBACK});
is(sql(q{SELECT count(*) FROM pg_dolt_ref WHERE refname='views/enabled'}), '0',
   'aborted first routine does not activate the database');
sql(q{CREATE FUNCTION f(i int) RETURNS int LANGUAGE sql AS 'SELECT i+1'});
sql(q{CREATE FUNCTION f(i text) RETURNS text LANGUAGE sql AS 'SELECT i'});
is(sql(q{SELECT count(*) FROM dolt_status}), '2', 'routine-only definitions appear in status');
sql(q{SELECT dolt_add('f(integer)')});
is(sql(q{SELECT count(*) FILTER (WHERE staged), count(*) FILTER (WHERE NOT staged) FROM dolt_status}),
   '1|1', 'named staging selects one normalized overload');
sql(q{SELECT dolt_commit('-m','one overload')});
is(sql(q{SET default_table_access_method=heap; SELECT f(4)}), '5',
   'activation survives a session changing its default access method');
sql(q{CREATE OR REPLACE FUNCTION f(i int) RETURNS int LANGUAGE sql AS 'SELECT i+10'});
sql(q{SELECT dolt_checkout('f(integer)')});
is(sql(q{SELECT f(4), f('text'::text)}), '5|text', 'restore preserves unrelated overload');
sql(q{SELECT dolt_clean('f(text)')});
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'clean removes only untracked overload');
sql(q{SELECT dolt_rm('f(integer)')});
is(sql(q{SELECT staged,status FROM dolt_status}), 't|deleted', 'rm stages routine deletion');
sql(q{SELECT dolt_reset('f(integer)')});
sql(q{SELECT dolt_checkout('f(integer)')});
is(sql(q{SELECT f(2)}), '3', 'deleted routine restored from HEAD');

sql(q{SELECT dolt_checkout('-b','other'); CREATE OR REPLACE FUNCTION f(i int) RETURNS int LANGUAGE sql AS 'SELECT i+20'; SELECT dolt_commit('-Am','other routine')});
sql(q{SELECT dolt_checkout('main')});
sql(q{CREATE OR REPLACE FUNCTION f(i int) RETURNS int LANGUAGE sql AS 'SELECT i+10'});
sql(q{SELECT dolt_commit('-Am','main routine')});
like(sql(q{SELECT dolt_merge('other')}), qr/conflicts found/, 'routine conflict returns status rather than aborting');
is(sql(q{SELECT f(1)}), '11', 'conflicted routine retains our working definition');
sql(q{SELECT dolt_conflicts_resolve('--theirs','f(integer)')});
is(sql(q{SELECT f(1)}), '21', 'routine conflict resolution normalizes signature aliases');
sql(q{SELECT dolt_commit('-Am','resolved')});
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'resolved routine merge commits cleanly');

sql(q{CREATE SCHEMA "sp ace"});
sql(q{CREATE FUNCTION "sp ace"."f.dot"(i int) RETURNS int LANGUAGE sql AS 'SELECT i'});
sql(q{SET search_path="sp ace",public; SELECT dolt_add('"f.dot"(int)')});
is(sql(q{SELECT count(*) FROM dolt_status WHERE staged AND table_name LIKE '%f.dot%'}), '1',
   'routine selectors honor quoted names and session search path');
$node->stop;
done_testing();
