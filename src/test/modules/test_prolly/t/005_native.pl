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
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_opclass WHERE opcmethod=(SELECT oid FROM pg_am WHERE amname='dolt_prolly_index') AND opcdefault}), '6', 'native default prolly opclasses present');
is($node->safe_psql('postgres', 'SHOW default_table_access_method'), 'heap', 'heap remains default');
is($node->safe_psql('postgres', q{SELECT string_agg(amname,',' ORDER BY amname) FROM pg_am WHERE amname NOT LIKE 'dolt_prolly_%'}), 'brin,btree,gin,gist,hash,heap,spgist', 'all upstream access methods retained');
$node->safe_psql('postgres', q{
CREATE TABLE normal(id int PRIMARY KEY, tags int[]);
CREATE INDEX normal_hash ON normal USING hash(id);
CREATE INDEX normal_gin ON normal USING gin(tags);
INSERT INTO normal VALUES (1,ARRAY[1,2]);
CREATE TABLE native(id int PRIMARY KEY, value text COLLATE "C") USING dolt_prolly_table;
INSERT INTO native VALUES(1,'one'),(2,'two');
CREATE ROLE reader;
GRANT SELECT ON native TO reader;
});
is($node->safe_psql('postgres', q{SELECT amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam WHERE c.oid='normal'::regclass}), 'heap', 'ordinary table uses heap');
is($node->safe_psql('postgres', q{SELECT amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam WHERE c.oid='normal_pkey'::regclass}), 'btree', 'ordinary primary key uses btree');
is($node->safe_psql('postgres', q{SELECT amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam WHERE c.oid='native_pkey'::regclass}), 'dolt_prolly_index', 'prolly primary key uses its own AM');
is($node->safe_psql('postgres', q{SET ROLE reader; SELECT value FROM native WHERE id=1}), 'one', 'normal SELECT privileges work');
my ($status, $out, $err) = $node->psql('postgres', q{SET ROLE reader; SELECT dolt_prolly_root('native')});
isnt($status, 0, 'root diagnostic remains restricted');
like($err, qr/permission denied for function dolt_prolly_root/, 'initdb revokes public diagnostic access');
my $root = $node->safe_psql('postgres', q{SELECT dolt_prolly_root('native')});

is($node->safe_psql('postgres', q{SELECT count(DISTINCT opcfamily) FROM pg_opclass WHERE opcmethod=(SELECT oid FROM pg_am WHERE amname='dolt_prolly_index') AND opcintype IN ('int2'::regtype,'int4'::regtype,'int8'::regtype)}), '1', 'integer opclasses share their cross-type family');
is($node->safe_psql('postgres', q{SET enable_seqscan=off; SELECT string_agg(value,',' ORDER BY id) FROM native WHERE id >= 1::bigint AND id <= 2::smallint}), 'one,two', 'cross-type integer index predicates work');
$node->safe_psql('postgres', 'ALTER TABLE native OWNER TO reader');
is($node->safe_psql('postgres', q{SELECT dolt_prolly_root('native')}), $root, 'ownership change preserves canonical data');

# pg_dump must treat these as built-ins, not dump CREATE ACCESS METHOD or an
# extension dependency. Restore into another fresh database without setup SQL.
my $dump = $node->basedir . '/native.dump';
command_ok(['pg_dump', '-Fc', '-h', $node->host, '-p', $node->port, '-d', 'postgres', '-f', $dump], 'dump mixed heap and native storage');
$node->safe_psql('postgres', 'CREATE DATABASE restored');
command_ok(['pg_restore', '-h', $node->host, '-p', $node->port, '-d', 'restored', '--exit-on-error', $dump], 'restore without CREATE EXTENSION');
is($node->safe_psql('restored', q{SELECT dolt_prolly_root('native')}), $root, 'restored canonical table root matches');
is($node->safe_psql('restored', q{SET enable_seqscan=off; SELECT string_agg(value,',' ORDER BY id) FROM native}), 'one,two', 'restored table and index work');
is($node->safe_psql('restored', q{SELECT amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam WHERE c.oid='native_pkey'::regclass}), 'dolt_prolly_index', 'restore reconstructs native primary key');
($status, $out, $err) = $node->psql('restored', q{INSERT INTO native VALUES (1,'duplicate')});
isnt($status, 0, 'restored primary key enforces uniqueness');
like($err, qr/duplicate key/, 'restored primary key reports uniqueness violation');
is($node->safe_psql('restored', q{SELECT tags FROM normal WHERE id=1}), '{1,2}', 'restored ordinary heap works');
is($node->safe_psql('restored', q{SELECT count(*) FROM pg_extension WHERE extname='dolt_prolly'}), '0', 'restore did not install an extension');
$node->stop;
done_testing();
