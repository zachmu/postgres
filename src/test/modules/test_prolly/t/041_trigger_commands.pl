# Nested trigger statements can advance the backend command ID before the
# outer table AM write. Its table and index edits still belong to one command.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('trigger_commands');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
$node->safe_psql('postgres', q{
  CREATE TABLE t(id int PRIMARY KEY, v text);
  INSERT INTO t VALUES (1, 'initial');
  CREATE TABLE log(id int, v text);
  CREATE FUNCTION log_write() RETURNS trigger LANGUAGE plpgsql AS $$
    BEGIN INSERT INTO log VALUES (OLD.id, OLD.v); RETURN NEW; END $$;
  CREATE TRIGGER logging BEFORE UPDATE ON t FOR EACH ROW EXECUTE FUNCTION log_write();
});
my $session = $node->background_psql('postgres');
$session->query_safe(q{SET enable_seqscan=off; SET enable_bitmapscan=off});
$session->query_safe(q{UPDATE t SET v='first' WHERE id=1});
is($session->query_safe(q{SELECT id,v FROM t WHERE id=1}), '1|first',
  'the index has one occurrence after a trigger advances the command ID');
$session->query_safe(q{UPDATE t SET v='second' WHERE id=1});
is($session->query_safe(q{SELECT id,v FROM t WHERE id=1}), '1|second',
  'a second update fetches the correct tuple');
$session->query_safe(q{UPDATE t SET id=2 WHERE id=1});
is($session->query_safe(q{SELECT id,v FROM t WHERE id IN (1,2)}), '2|second',
  'an indexed column change removes the old key');
is($session->query_safe(q{SELECT count(*) FROM log}), '3', 'each update runs its trigger once');
$session->quit;
$node->restart;
is($node->safe_psql('postgres', q{SET enable_seqscan=off; SELECT id,v FROM t WHERE id=2}),
  '2|second', 'the index remains correct after restart');
$node->stop;
done_testing();
