# Trigger order is alphabetical, including after catalog reload and branch edits.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('trigger_order');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE TABLE watched(id int PRIMARY KEY, value text);
      CREATE FUNCTION append_name() RETURNS trigger LANGUAGE plpgsql AS $$
      BEGIN NEW.value := NEW.value || TG_NAME; RETURN NEW; END $$;
      CREATE TRIGGER c BEFORE INSERT OR UPDATE ON watched FOR EACH ROW EXECUTE FUNCTION append_name();
      CREATE TRIGGER a BEFORE INSERT OR UPDATE ON watched FOR EACH ROW EXECUTE FUNCTION append_name();
      CREATE TRIGGER b BEFORE INSERT OR UPDATE ON watched FOR EACH ROW EXECUTE FUNCTION append_name()});
is(sql(q{INSERT INTO watched VALUES (1,'') RETURNING value}), 'abc',
   'insert triggers fire in name order after catalog reload');
is(sql(q{UPDATE watched SET value='' RETURNING value}), 'abc',
   'update triggers fire in name order');
sql(q{SELECT dolt_commit('-Am','ordered triggers'); SELECT dolt_branch('other')});
like(sql(q{SELECT dolt_checkout('other'); ALTER TRIGGER a ON watched RENAME TO z;
         UPDATE watched SET value='' RETURNING value}),
   qr/bcz$/,  'renamed trigger changes branch-local firing order');
is(sql(q{UPDATE watched SET value='' RETURNING value}), 'abc',
   'main branch retains original trigger order');
$node->restart;
like(sql(q{SELECT dolt_checkout('other'); UPDATE watched SET value='' RETURNING value}),
   qr/bcz$/,  'trigger order survives restart');
$node->stop;
done_testing();
