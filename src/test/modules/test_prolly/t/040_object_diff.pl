use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('object_diff');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub fails {
 my ($query, $pattern, $label) = @_;
 my ($status, $out, $err) = $node->psql('postgres', $query);
 isnt($status, 0, $label);
 like($err, $pattern, "$label diagnostic");
}

sql(q{
 CREATE SCHEMA "Diff Schema";
 CREATE FUNCTION "Diff Schema"."f.x"(i integer) RETURNS integer LANGUAGE sql AS 'SELECT i+1';
 CREATE FUNCTION "Diff Schema"."f.x"(i text) RETURNS text LANGUAGE sql AS 'SELECT i';
 CREATE VIEW v AS SELECT 1 AS n;
 CREATE TYPE mood AS ENUM ('a');
 CREATE TABLE t(n int);
 CREATE FUNCTION trfn() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
 CREATE TRIGGER tr BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trfn();
 CREATE PROCEDURE p() LANGUAGE plpgsql AS 'BEGIN NULL; END';
});
sql(q{SELECT dolt_commit('-Am','base')});
sql(q{
 CREATE OR REPLACE FUNCTION "Diff Schema"."f.x"(i integer) RETURNS integer LANGUAGE sql AS 'SELECT i+2';
 CREATE OR REPLACE VIEW v AS SELECT 2 AS n;
 ALTER TYPE mood ADD VALUE 'b';
 ALTER TABLE t DISABLE TRIGGER tr;
 CREATE OR REPLACE PROCEDURE p() LANGUAGE plpgsql AS 'BEGIN PERFORM 1; END';
});
is(sql(q{SELECT from_value #>> '{}',to_value #>> '{}' FROM dolt_diff('HEAD','WORKING','"Diff Schema"."f.x"(integer)')}),
 'SELECT i+1|SELECT i+2', 'quoted routine and canonical type alias');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD','WORKING','"Diff Schema"."f.x"(text)')}),
 '0', 'unchanged overload is independent');
is(sql(q{SET search_path="Diff Schema"; SELECT count(*) FROM dolt_diff('HEAD','WORKING','"f.x"(int4)')}),
 '1', 'routine lookup follows caller search_path');
is(sql(q{SELECT from_property,from_value,to_value FROM dolt_diff('HEAD','WORKING','mood')}),
 'labels|["a"]|["a", "b"]', 'nested enum labels retain JSON type');
is(sql(q{SELECT from_property,btrim(from_value #>> '{}'),btrim(to_value #>> '{}') FROM dolt_diff('HEAD','WORKING','v')}),
 'query|SELECT 1 AS n|SELECT 2 AS n', 'view definition properties');
is(sql(q{SELECT from_property,from_value #>> '{}',to_value #>> '{}' FROM dolt_diff('HEAD','WORKING','t.tr')}),
 'enabled|O|D', 'table-qualified trigger properties');
is(sql(q{SELECT from_property,from_value #>> '{}',to_value #>> '{}' FROM dolt_diff('HEAD','WORKING','p()')}),
 'prosrc|BEGIN NULL; END|BEGIN PERFORM 1; END', 'procedures use routine property model');
is(sql(q{SELECT n FROM v}), '2', 'native view execution agrees with new definition');
is(sql(q{SELECT table_name,rows_modified,old_row_count,new_row_count FROM dolt_diff_stat('HEAD','WORKING') ORDER BY 1}),
 "\"Diff Schema\".\"f.x\"(pg_catalog.int4)|1|0|0\npublic.mood|1|0|0\npublic.p()|1|0|0\npublic.t/tr|1|0|0\npublic.v|1|0|0", 'all changed object kinds appear in statistics');

# Read-only means no materialization or publication, including during planning.
my $objects = sql(q{SELECT count(*) FROM pg_dolt_object});
my $relations = sql(q{SELECT count(*) FROM pg_class});
is(sql(q{BEGIN READ ONLY; SELECT count(*) FROM dolt_diff('HEAD','WORKING','v'); COMMIT}), '1',
 'property diff works in read-only transaction');
is(sql(q{SELECT count(*) FROM pg_dolt_object}), $objects, 'diff creates no durable objects');
is(sql(q{SELECT count(*) FROM pg_class}), $relations, 'diff creates no physical catalog relations');

sql(q{SELECT dolt_add('mood')});
is(sql(q{SELECT count(*) FROM dolt_diff('STAGED','WORKING','mood')}), '0', 'staged property reads match working');
sql(q{SELECT dolt_commit('-Am','modified')});
my $session = $node->background_psql('postgres');
$session->query_safe(q{SELECT dolt_checkout('-b','feature')});
$session->query_safe(q{CREATE OR REPLACE VIEW v AS SELECT 3 AS n});
is($session->query_safe(q{SELECT btrim(from_value #>> '{}'),btrim(to_value #>> '{}') FROM dolt_diff('main','WORKING','v')}),
 'SELECT 2 AS n|SELECT 3 AS n', 'other-branch retained definitions');
$session->query_safe(q{SELECT dolt_commit('-Am','feature')});
$session->quit;
is(sql(q{SELECT btrim(from_value #>> '{}'),btrim(to_value #>> '{}') FROM dolt_diff('main...feature','v')}),
 'SELECT 2 AS n|SELECT 3 AS n', 'three-dot revision range');
sql(q{DROP VIEW v});
is(sql(q{SELECT to_property IS NULL,from_property,diff_type FROM dolt_diff('HEAD','WORKING','v') WHERE from_property='query'}),
 't|query|removed', 'absent object has NULL from/to layout');
is(sql(q{SELECT rows_unmodified,rows_deleted,old_row_count,new_row_count FROM dolt_diff_stat('HEAD','WORKING','v')}),
 '-1|1|0|0', 'object deletion matches Dolt totals');
fails(q{SELECT * FROM dolt_diff('HEAD','WORKING','missing')}, qr/table not found/, 'missing selector rejected');
fails(q{SELECT * FROM dolt_diff('missing_revision','WORKING','mood')}, qr/revision|branch|commit/, 'missing revision rejected');
sql(q{CREATE TABLE v(n int)});
fails(q{SELECT * FROM dolt_diff('HEAD','WORKING','v')}, qr/object kind changed/, 'table versus view diff rejected');
fails(q{SELECT * FROM dolt_diff_stat('HEAD','WORKING','v')}, qr/object kind changed/, 'table versus view statistics rejected');
# Signatures must resolve their types from history, even after both are gone.
sql(q{CREATE TYPE gone AS ENUM ('x'); CREATE FUNCTION typed(x gone[]) RETURNS int LANGUAGE sql AS 'SELECT 1';
 CREATE TYPE public.int4 AS ENUM ('shadow');
 CREATE FUNCTION native_arg(x pg_catalog.int4) RETURNS int LANGUAGE sql AS 'SELECT 1'});
sql(q{SELECT dolt_commit('-Am','typed')});
sql(q{DROP FUNCTION typed(gone[]); DROP TYPE gone;
 CREATE OR REPLACE FUNCTION native_arg(x pg_catalog.int4) RETURNS int LANGUAGE sql AS 'SELECT 2'});
is(sql(q{SELECT to_value #>> '{}' FROM dolt_diff('HEAD','WORKING','native_arg(int4)')}),
 'SELECT 2', 'implicit pg_catalog type precedence is preserved');
is(sql(q{SELECT from_value #>> '{}',diff_type FROM dolt_diff('HEAD','WORKING','typed(gone[])') WHERE from_property='prosrc'}),
 'SELECT 1|removed', 'historical signature resolves a dropped argument type');
is(sql(q{SELECT rows_deleted FROM dolt_diff_stat('HEAD','WORKING','typed(gone[])')}),
 '1', 'statistics resolve dropped argument types');
is(sql(q{SELECT to_value IS NOT NULL, jsonb_typeof(to_value), from_property IS NULL FROM dolt_diff('WORKING','HEAD','typed(gone[])') WHERE to_property='proconfig'}),
 't|null|t', 'JSON null property differs from SQL NULL on absent side');
$node->restart;
is(sql(q{SELECT from_value,to_value FROM dolt_diff('HEAD~2','HEAD~1','mood')}),
 '["a"]|["a", "b"]', 'retained property diffs survive restart');
$node->stop;
done_testing();
