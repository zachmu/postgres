# Array keys use PostgreSQL element, dimensionality and lower-bound ordering.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('array_indexes');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
my $force = 'SET enable_seqscan=off; SET enable_bitmapscan=off; ';
sql(q{CREATE TABLE p(id int, a text[] COLLATE "C") USING dolt_prolly_table;
      INSERT INTO p VALUES (1,'{}'),(2,'{a}'),(3,'{a,NULL}'),(4,'{a,b}'),
      (5,'{NULL}'),(6,'{{a,b},{c,d}}'),(7,'{a,b,c,d}'),(8,'[0:1]={a,b}'),
      (9,'{"",a}'),(10,'{aa}'),(11,NULL),(12,'[2:3]={a,b}');
      CREATE INDEX p_array ON p(a);
      CREATE TABLE h(id int, a text[] COLLATE "C") USING heap;
      INSERT INTO h TABLE p; CREATE INDEX h_array ON h(a)});
for my $predicate ('true', q{a = '{a,b}'}, q{a < '{a,b}'}, q{a >= '{a,b}'},
                   q{a = '{a,NULL}'}, q{a = '[0:1]={a,b}'}, 'a IS NULL',
                   q{a = '{}'}, q{a > '{NULL}'})
{
 is(sql($force."SELECT id FROM p WHERE $predicate ORDER BY a NULLS LAST,id"),
    sql("SELECT id FROM h WHERE $predicate ORDER BY a NULLS LAST,id"),
    "array index matches heap: $predicate");
}
like(sql($force.q{EXPLAIN (COSTS OFF) SELECT * FROM p WHERE a='{a,b}'}),
     qr/Index Scan/, 'array equality actually uses index access');
sql(q{CREATE TABLE unique_arrays(a numeric[] PRIMARY KEY) USING dolt_prolly_table;
      INSERT INTO unique_arrays VALUES ('{1.0,NULL}'),('{2,3}')});
my ($ret,$out,$err) = $node->psql('postgres', q{INSERT INTO unique_arrays VALUES ('{1.00,NULL}')});
like($err, qr/duplicate key/, 'array uniqueness uses element equality including NULL elements');
sql(q{UPDATE unique_arrays SET a='{4,5}' WHERE a='{2,3}';
      DELETE FROM unique_arrays WHERE a='{1,NULL}'});
is(sql($force.q{SELECT a FROM unique_arrays WHERE a='{4,5}'}), '{4,5}', 'array update/delete maintains index');
sql(q{CREATE TABLE ints(a int[]) USING dolt_prolly_table;
      INSERT INTO ints VALUES ('{-1,0}'),('{1}'),('{0,-2}'),('{{0,0}}'),('[0:0]={1}');
      CREATE INDEX ints_desc ON ints(a DESC)});
is(sql($force.q{SELECT a FROM ints ORDER BY a DESC}),
   sql(q{SET enable_indexscan=off; SET enable_indexonlyscan=off; SELECT a FROM ints ORDER BY a DESC}),
   'descending integer array order matches native sort');
sql(q{SELECT dolt_commit('-Am','array keys'); SELECT dolt_branch('other')});
sql(q{SELECT dolt_checkout('other'); INSERT INTO unique_arrays VALUES ('{9}')});
is(sql($force.q{SELECT a FROM "postgres/other".public.unique_arrays WHERE a='{9}'}), '{9}', 'branch array index sees branch row');
is(sql($force.q{SELECT count(*) FROM unique_arrays WHERE a='{9}'}), '0', 'branch index writes stay isolated');
$node->restart;
is(sql($force.q{SELECT id FROM p AS OF 'HEAD' WHERE a='{a,NULL}'}), '3', 'historical array lookup survives restart');
$node->stop;
done_testing();
