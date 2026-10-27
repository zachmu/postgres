# System view expansion must preserve the branch as well as its schema hash.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('catalog_view_joins');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sql(q{CREATE TABLE indexed(id int PRIMARY KEY, value int);
      CREATE INDEX value_idx ON indexed(value);
      SELECT dolt_commit('-Am','indexes'); SELECT dolt_branch('other')});
my $join = q{SELECT c.relname FROM pg_stat_all_indexes s JOIN pg_class c
             ON c.oid=s.indexrelid WHERE s.relname='indexed' ORDER BY c.relname};
is(sql($join), "indexed_pkey\nvalue_idx", 'system view index OIDs join to current catalog');
is(sql(q{SELECT count(*) FROM pg_stat_user_indexes s JOIN pg_class c ON c.oid=s.relid
         WHERE s.relname='indexed'}), '2', 'nested system view retains current branch');
is(sql(q{SELECT count(*) FROM pg_stat_all_indexes AS OF 'HEAD' s
         JOIN pg_class AS OF 'HEAD' c ON c.oid=s.indexrelid WHERE s.relname='indexed'}),
   '2', 'historical system view and catalog share revision handles');
sql(q{SELECT dolt_checkout('other'); CREATE INDEX other_idx ON indexed(id,value)});
is(sql(q{SELECT c.relname FROM "postgres/other".pg_catalog.pg_stat_all_indexes s
         JOIN "postgres/other".pg_catalog.pg_class c ON c.oid=s.indexrelid
         WHERE s.relname='indexed' ORDER BY c.relname}),
   "indexed_pkey\nother_idx\nvalue_idx", 'explicit other-branch system view retains branch handles');
is(sql($join), "indexed_pkey\nvalue_idx", 'other branch index stays isolated');
$node->restart;
is(sql($join), "indexed_pkey\nvalue_idx", 'catalog joins survive restart');
$node->stop;
done_testing();
