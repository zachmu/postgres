# Cross-object row types must be loadable before expressions consume them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('rowtype_reload');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

# The composite is committed separately. Each sql() opens a fresh backend;
# previously the successful CREATE TYPE left all later table reads broken.
sql(q{CREATE TABLE source_row(id int PRIMARY KEY, value text);
      INSERT INTO source_row VALUES (1,'one')});
sql(q{CREATE TYPE envelope AS (item source_row, items source_row[])});
is(sql(q{SELECT value FROM source_row}), 'one',
   'table remains readable after a composite references its row type');
is(sql(q{SELECT ((ROW(s,ARRAY[s])::envelope).item).value FROM source_row s}), 'one',
   'nested table row type resolves in a composite');
is(sql(q{SELECT ((ROW(s,ARRAY[s])::envelope).items)[1].value FROM source_row s}), 'one',
   'table row-type array resolves in a composite');
sql(q{CREATE TYPE envelope2 AS (inner_value envelope)});
is(sql(q{SELECT (((ROW(ROW(s,ARRAY[s])::envelope)::envelope2).inner_value).item).value FROM source_row s}),
   'one', 'multiple composite levels can be reconstructed');
sql(q{CREATE VIEW wrapped AS SELECT ROW(s,ARRAY[s])::envelope AS payload FROM source_row s});
is(sql(q{SELECT (payload).item.value FROM wrapped}), 'one', 'view binds nested row types');

# Test whole-row conditions, not just OLD.column / NEW.column. A successful
# CREATE TRIGGER previously made later catalog loads fail, even after restart.
sql(q{CREATE TABLE watched(id int PRIMARY KEY, value text);
      INSERT INTO watched VALUES (1,'old');
      CREATE TABLE events(value text);
      CREATE FUNCTION record_change() RETURNS trigger LANGUAGE plpgsql AS $$
      BEGIN INSERT INTO events VALUES (NEW.value); RETURN NEW; END $$});
sql(q{CREATE TRIGGER whole_row AFTER UPDATE ON watched FOR EACH ROW
      WHEN (OLD.* IS DISTINCT FROM NEW.*) EXECUTE FUNCTION record_change()});
is(sql(q{SELECT value FROM watched}), 'old', 'whole-row condition reloads in a new session');
sql(q{UPDATE watched SET value='old'; UPDATE watched SET value='new'});
is(sql(q{SELECT string_agg(value,',' ORDER BY value) FROM events}), 'new',
   'whole-row trigger fires only when a row changes');
like(sql(q{SELECT pg_get_triggerdef(oid) FROM pg_trigger WHERE tgname='whole_row'}),
     qr/old\.\* IS DISTINCT FROM new\.\*/i, 'whole-row condition deparses');
sql(q{SELECT dolt_commit('-Am','row types and whole-row trigger'); SELECT dolt_branch('other')});
is(sql(q{SELECT (payload).item.value FROM wrapped AS OF 'HEAD'}), 'one',
   'historical view binds its nested row types');
is(sql(q{SELECT value FROM watched AS OF 'HEAD'}), 'new',
   'historical table catalog loads its whole-row trigger');
sql(q{SELECT dolt_checkout('other'); ALTER TABLE watched ADD COLUMN note text;
      UPDATE watched SET value='other'; UPDATE watched SET note='changed';
      ALTER TABLE source_row ADD COLUMN extra int; UPDATE source_row SET extra=7});
is(sql(q{SELECT value FROM watched}), 'new', 'other branch update leaves main unchanged');
is(sql(q{SELECT value FROM "postgres/other".public.watched}), 'other',
   'other branch executes its whole-row trigger');
is(sql(q{SELECT string_agg(value,',' ORDER BY value) FROM "postgres/other".public.events}),
   'new,other,other', 'whole-row comparison includes a branch-only column');
is(sql(q{SELECT (m.payload).item.value||':'||(o.payload).item.extra
         FROM wrapped m CROSS JOIN "postgres/other".public.wrapped o}),
   'one:7', 'one query binds nested row types with different branch layouts');

$node->restart;
is(sql(q{SELECT (payload).item.value FROM wrapped}), 'one', 'nested row types survive restart');
is(sql(q{SELECT value FROM watched}), 'new', 'whole-row condition survives restart');
sql(q{UPDATE watched SET value='after restart'});
is(sql(q{SELECT string_agg(value,',' ORDER BY value) FROM events}), 'after restart,new',
   'whole-row condition executes after restart');
is(sql(q{SELECT (payload).item.value FROM wrapped AS OF 'HEAD'}), 'one',
   'historical nested row types reload after restart');
is(sql(q{SELECT value FROM watched AS OF 'HEAD'}), 'new', 'historical trigger catalog reloads after restart');
$node->stop;
done_testing();
