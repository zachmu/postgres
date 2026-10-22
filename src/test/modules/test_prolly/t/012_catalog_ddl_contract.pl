# Safety boundaries for the branch-local DDL implementation gate. These tests
# establish current behavior; rejected DDL is not evidence of DDL support.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('catalog_ddl_contract');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub rejected
{
 my ($query, $pattern, $label) = @_;
 my ($status, $out, $err) = $node->psql('postgres', $query);
 isnt($status, 0, $label);
 like($err, $pattern, "$label diagnostic");
}

# A SQL transaction snapshot does not version ordinary relcache definitions.
# Establish the snapshot without opening the target (which would lock out DDL).
sql('CREATE TABLE catalog_control(a int) USING heap');
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe('SELECT count(*) FROM pg_class');
sql('ALTER TABLE catalog_control ADD COLUMN b text');
is($reader->query_safe(q{SELECT count(*) FROM pg_attribute WHERE attrelid='catalog_control'::regclass AND attnum>0}),
 '1', 'SQL catalog scan retains the old transaction snapshot');
is($reader->query_safe(q{SELECT pg_typeof(b) FROM catalog_control RIGHT JOIN (VALUES(1)) dummy(x) ON true}),
 'text', 'parser/relcache can bind the newer definition under the same transaction');
$reader->query_safe('COMMIT');
$reader->quit;
sql('DROP TABLE catalog_control');

sql('CREATE TABLE t(id int PRIMARY KEY,v int) USING dolt_prolly_table; INSERT INTO t VALUES(1,10)');
my $old = sql(q{SELECT dolt_commit('-Am','base')});
my $before = sql(q{SELECT encode(reftarget,'hex') FROM pg_dolt_ref WHERE refname='heads/main'});
rejected('ALTER TABLE t ADD COLUMN extra int', qr/schema versioning is not implemented/,
 'layout changes guarded even before branch activation');
is(sql('SELECT * FROM t'), '1|10', 'rejected ALTER preserves existing tuple decoding');

# Relation identity can change while canonical history remains valid, but current
# single-branch CREATE/DROP rollback must not leak any replacement definition.
sql('BEGIN; DROP TABLE t; CREATE TABLE t(id int,v text) USING dolt_prolly_table; INSERT INTO t VALUES(2,\'new\'); ROLLBACK');
is(sql('SELECT * FROM t'), '1|10', 'DROP and incompatible CREATE rollback restores original live table');
is(sql("SELECT * FROM t AS OF '$old'"), '1|10', 'retained descriptor and data survive aborted replacement');
is(sql(q{SELECT encode(reftarget,'hex') FROM pg_dolt_ref WHERE refname='heads/main'}),
 $before, 'ordinary DDL rollback does not move HEAD');

sql(q{SELECT dolt_branch('other'); SELECT dolt_checkout('other')});
for my $case (
 ['CREATE TABLE branch_only(id int) USING dolt_prolly_table', 'CREATE'],
 ['ALTER TABLE t ADD COLUMN extra int', 'ALTER'],
 ['TRUNCATE t', 'TRUNCATE'])
{
 rejected(q{SELECT dolt_checkout('other'); } . $case->[0], qr/schema changes with multiple branches/, "$case->[1] remains guarded on activated branches");
}
is(sql(q{SELECT * FROM "postgres/other".public.t}), '1|10', 'failed branch DDL leaves selected branch data intact');
is(sql(q{SELECT * FROM "postgres/main".public.t}), '1|10', 'failed branch DDL leaves other branch intact');
is(sql(q{SELECT to_regclass('branch_only') IS NULL}), 't', 'failed CREATE leaves no shared catalog object');

# A failing DDL statement must release only its subtransaction work, not erase
# earlier writes; publication still uses the existing transaction outcome.
# Use one backend for the savepoint sequence.
my $writer = $node->background_psql('postgres', on_error_stop => 0);
$writer->query_safe(q{SELECT dolt_checkout('other')});
$writer->query_safe('BEGIN; UPDATE t SET v=20; SAVEPOINT ddl');
my ($out, $err) = $writer->query('ALTER TABLE t ADD COLUMN extra int');
is($err, 1, 'DDL fails inside savepoint');
$writer->query('ROLLBACK TO ddl');
is(scalar $writer->query('SELECT v FROM t'), '20', 'savepoint failure preserves preceding own write');
is(sql(q{SELECT v FROM "postgres/other".public.t}), '10', 'uncommitted write is invisible to another session');
$writer->query('COMMIT');
$writer->quit;
is(sql(q{SELECT v FROM "postgres/other".public.t}), '20', 'committed working root becomes visible');
is(sql(q{SELECT v FROM "postgres/main".public.t}), '10', 'other branch root remains unchanged');
$node->stop('immediate');
$node->start;
rejected('ALTER TABLE t ADD COLUMN extra int', qr/schema changes with multiple branches/, 'DDL activation guard survives restart');
is(sql("SELECT * FROM t AS OF '$old'"), '1|10', 'retained schema survives restart');
$node->stop;
done_testing();
