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
# SQL namespace identities must be the same handles exposed by the catalogs.
sql(q{CREATE SCHEMA ns_identity; CREATE TABLE ns_identity.t(id int PRIMARY KEY)});
is(sql(q{SELECT n.oid='ns_identity'::regnamespace AND
    n.oid=to_regnamespace('ns_identity') AND c.relnamespace=n.oid
    FROM pg_namespace n JOIN pg_class c ON c.relnamespace=n.oid
    WHERE n.nspname='ns_identity' AND c.relname='t'}), 't',
    'regnamespace casts and catalog joins use one namespace identity');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE connamespace='ns_identity'::regnamespace
    AND contype='p'}), '1', 'namespace cast filters find constraints');
is(sql(q{SELECT 'ns_identity'::regnamespace::text}), 'ns_identity', 'namespace handle prints its name');
is(sql(q{SELECT to_regnamespace('missing_schema') IS NULL}), 't', 'missing namespace has native NULL result');
is(sql(q{SELECT '12345'::regnamespace::oid}), '12345', 'numeric namespace input remains literal');
is(sql(q{PREPARE ns_lookup AS SELECT count(*) FROM pg_class
    WHERE relnamespace='ns_identity'::regnamespace AND relname='t'; EXECUTE ns_lookup; EXECUTE ns_lookup}),
    "1\n1", 'prepared catalog queries retain the namespace context');
sql(q{SELECT dolt_commit('-Am','namespace identities')});
is(sql(q{SELECT oid::regnamespace::text FROM pg_namespace AS OF 'HEAD' WHERE nspname='ns_identity'}),
    'ns_identity', 'historical namespace handles retain their names');
is(sql(q{SELECT dolt_checkout('other'); CREATE SCHEMA branch_only;
    SELECT oid=to_regnamespace('branch_only') FROM pg_namespace WHERE nspname='branch_only'}),
    "(0,\"Switched to branch 'other'\")\nt", 'another session branch resolves its own namespace');
is(sql(q{SELECT to_regnamespace('branch_only') IS NULL}), 't', 'another branch namespace stays hidden');

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
