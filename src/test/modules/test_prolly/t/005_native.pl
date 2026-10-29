use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('native');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\n");
$node->start;
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_extension WHERE extname='dolt_prolly'}), '0', 'no extension installed');
is($node->safe_psql('postgres', q{SELECT string_agg(amname,',' ORDER BY amname) FROM pg_am WHERE amname LIKE 'dolt_prolly_%'}), 'dolt_prolly_index,dolt_prolly_table', 'native AMs present after initdb');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang WHERE p.proname IN ('dolt_prolly_table_handler','dolt_prolly_index_handler','dolt_prolly_root') AND l.lanname='internal' AND p.pronamespace='pg_catalog'::regnamespace AND p.probin IS NULL}), '3', 'handlers and diagnostic linked into backend');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_opclass WHERE opcmethod=(SELECT oid FROM pg_am WHERE amname='dolt_prolly_index') AND opcdefault}), '32', 'native default prolly opclasses present');
is($node->safe_psql('postgres', 'SHOW default_table_access_method'), 'heap', 'heap remains default');
is($node->safe_psql('postgres', q{SELECT string_agg(amname,',' ORDER BY amname) FROM pg_am WHERE amname NOT LIKE 'dolt_prolly_%'}), 'brin,btree,gin,gist,hash,heap,spgist', 'all upstream access methods retained');
$node->safe_psql('postgres', q{
CREATE TABLE normal(id int PRIMARY KEY, tags int[]);
CREATE INDEX normal_hash ON normal USING hash(id);
CREATE INDEX normal_gin ON normal USING gin(tags);
INSERT INTO normal VALUES (1,ARRAY[1,2]);
CREATE TABLE native(id int PRIMARY KEY, value text COLLATE "C") USING dolt_prolly_table;
INSERT INTO native VALUES(1,'one'),(2,'two');
CREATE ROLE reader IN ROLE pg_read_all_data;
});
# TODO: SQL queries of catalogs on a branch show versioned and built-in objects
# but not native ones. Check the native table through its relcache meanwhile.
is($node->safe_psql('postgres', q{SELECT tags FROM normal}), '{1,2}', 'ordinary heap table coexists with versioned tables');
is($node->safe_psql('postgres', q{SELECT amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam WHERE c.oid='native_pkey'::regclass}), 'dolt_prolly_index', 'prolly primary key uses its own AM');
is($node->safe_psql('postgres', q{SET ROLE reader; SELECT value FROM native WHERE id=1}), 'one', 'non-superuser reads work');
my ($status, $out, $err) = $node->psql('postgres', q{SET ROLE reader; SELECT dolt_prolly_root('native')});
isnt($status, 0, 'root diagnostic remains restricted');
like($err, qr/permission denied for function dolt_prolly_root/, 'initdb revokes public diagnostic access');
my $root = $node->safe_psql('postgres', q{SELECT dolt_prolly_root('native')});

is($node->safe_psql('postgres', q{SELECT count(DISTINCT opcfamily) FROM pg_opclass WHERE opcmethod=(SELECT oid FROM pg_am WHERE amname='dolt_prolly_index') AND opcintype IN ('int2'::regtype,'int4'::regtype,'int8'::regtype)}), '1', 'integer opclasses share their cross-type family');
is($node->safe_psql('postgres', q{SET enable_seqscan=off; SELECT string_agg(value,',' ORDER BY id) FROM native WHERE id >= 1::bigint AND id <= 2::smallint}), 'one,two', 'cross-type integer index predicates work');
# TODO: ownership changes of versioned tables, once the ownership and ACL
# design settles who owns a versioned table.

# TODO: pg_dump and pg_restore of a repository. Branch catalog scans lack
# system columns such as tableoid, and dumping versioned tables needs a design.
$node->stop;
done_testing();
