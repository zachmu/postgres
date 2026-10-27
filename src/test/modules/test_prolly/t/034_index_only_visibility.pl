# Index-only execution must not interpret prolly TIDs as heap page addresses.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('index_only_visibility');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
my $force = 'SET enable_seqscan=off; SET enable_bitmapscan=off; ';
sql(q{CREATE TABLE p(id int PRIMARY KEY, note text) USING dolt_prolly_table;
      INSERT INTO p VALUES (1,'one'),(2,'two');
      SELECT dolt_commit('-Am','initial'); SELECT dolt_branch('other')});
for my $source ('p', q{p AS OF 'HEAD'}, q{"postgres/other".public.p})
{
 like(sql($force."EXPLAIN (COSTS OFF) SELECT id FROM $source ORDER BY id"),
      qr/Index Only Scan/, "index-only plan selected: $source");
 is(sql($force."SELECT id FROM $source ORDER BY id"), "1\n2", "index-only read: $source");
}
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; '.$force);
is($reader->query_safe('SELECT id FROM p ORDER BY id'), "1\n2", 'reader establishes snapshot');
sql(q{DELETE FROM p WHERE id=1; INSERT INTO p VALUES (3,'three')});
is($reader->query_safe('SELECT id FROM p ORDER BY id'), "1\n2", 'index-only scan retains older snapshot');
$reader->query_safe('ROLLBACK');
$reader->quit;
is(sql($force.'SELECT id FROM p ORDER BY id'), "2\n3", 'new snapshot sees current roots');
is(sql($force.q{SELECT id FROM p AS OF 'HEAD' ORDER BY id}), "1\n2", 'historical index retains old rows');
sql(q{SELECT dolt_checkout('other'); INSERT INTO p VALUES (4,'four')});
is(sql($force.q{SELECT id FROM "postgres/other".public.p ORDER BY id}), "1\n2\n4", 'other branch has its own index-only view');
$node->restart;
is(sql($force.q{SELECT id FROM p AS OF 'HEAD' ORDER BY id}), "1\n2", 'historical index-only scan survives restart');
$node->stop;
done_testing();
