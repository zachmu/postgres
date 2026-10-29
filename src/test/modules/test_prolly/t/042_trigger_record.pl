# SELECT INTO in a BEFORE INSERT trigger advances command IDs too. Inserting
# once must not add two index occurrences, even after a subsequent error.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('trigger_record');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
$node->safe_psql('postgres', q{
  CREATE TABLE t(id int PRIMARY KEY, v int);
  CREATE FUNCTION guard() RETURNS trigger LANGUAGE plpgsql AS $$
    DECLARE blocker RECORD;
    BEGIN
      SELECT id,v INTO blocker FROM t WHERE v > NEW.v LIMIT 1;
      IF blocker.id IS NOT NULL THEN
        RAISE EXCEPTION 'blocked by row %', blocker.id;
      END IF;
      RETURN NEW;
    END $$;
  CREATE TRIGGER guard BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION guard();
});
my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe(q{INSERT INTO t VALUES (1,10)});
is($session->query_safe(q{SELECT id,v FROM t ORDER BY id}), '1|10',
  'one index occurrence after the trigger SELECT INTO');
$session->query(q{INSERT INTO t VALUES (2,5)});
like($session->{stderr}, qr/blocked by row 1/, 'the trigger rejects the smaller value');
$session->{stderr} = '';
is($session->query_safe(q{SELECT id,v FROM t ORDER BY id}), '1|10',
  'the failed insertion does not duplicate the original row');
$session->query_safe(q{INSERT INTO t VALUES (2,20)});
is($session->query_safe(q{SELECT id,v FROM t ORDER BY id}), "1|10\n2|20",
  'a later accepted insertion appears once');
$session->query_safe(q{SET enable_indexscan=off; SET enable_bitmapscan=off});
is($session->query_safe(q{SELECT id,v FROM t ORDER BY id}), "1|10\n2|20",
  'table and index scans agree');
$session->quit;
$node->stop;
done_testing();
