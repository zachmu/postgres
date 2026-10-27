# Whole-row catalog planning and nested SQL DROP must honor native contracts.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('catalog_read_safety');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sql(q{CREATE TABLE test_table(id int PRIMARY KEY, col1 varchar(255));
      CREATE VIEW first_view AS SELECT * FROM test_table;
      CREATE VIEW second_view AS SELECT * FROM test_table;
      CREATE FUNCTION drop_views() RETURNS void LANGUAGE sql AS $$
        DROP VIEW first_view; DROP VIEW second_view;
      $$});
like(sql(q{SELECT * FROM information_schema.columns WHERE table_name='test_table' ORDER BY ordinal_position}),
     qr/test_table\|col1/, 'full information_schema.columns scan plans and executes');
like(sql(q{SELECT c FROM pg_class c WHERE relname='test_table'}), qr/test_table/,
     'whole-row canonical catalog scan holds its bootstrap descriptor lock');
my $session = $node->background_psql('postgres');
$session->query_safe(q{PREPARE catalog_rows AS SELECT c FROM pg_class c WHERE relname='test_table'});
like($session->query_safe('EXECUTE catalog_rows'), qr/test_table/, 'prepared whole-row catalog plan executes');
like($session->query_safe('EXECUTE catalog_rows'), qr/test_table/, 'prepared plan reuses descriptor safely');
$session->quit;
sql(q{BEGIN; SELECT drop_views(); ROLLBACK});
is(sql(q{SELECT count(*) FROM pg_class WHERE relname IN ('first_view','second_view')}),
   '2', 'nested SQL DROP rolls back atomically');
sql(q{SELECT drop_views()});
is(sql(q{SELECT count(*) FROM pg_class WHERE relname IN ('first_view','second_view')}),
   '0', 'SQL function drops both views without mutating its registered snapshot');
$node->stop;
done_testing();
