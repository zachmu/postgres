use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('schematables');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sub fails {
 my($q,$pat,$label)=@_;my($status,$out,$err)=$node->psql('postgres',$q);
 isnt($status,0,$label);like($err,$pat,"$label diagnostic");
}
# ---- dolt_ignore: a per-schema table created by its first INSERT ----
is(sql(q{SELECT * FROM dolt_ignore}),'','absent dolt_ignore reads as empty');
is(sql(q{SELECT count(*) FROM pg_class WHERE relname='dolt_ignore'}),'0','no table exists until an insert');
sql(q{INSERT INTO dolt_ignore VALUES ('generated_*', true), ('generated_exception', false)});
is(sql(q{SELECT * FROM dolt_ignore ORDER BY pattern}),"generated_*|t\ngenerated_exception|f",'insert creates the table');
is(sql(q{SELECT dolt_ignore.pattern FROM public.dolt_ignore WHERE ignored=false}),'generated_exception','qualified and aliased access');
is(sql(q{SELECT relam=(SELECT oid FROM pg_am WHERE amname='dolt_prolly_table') FROM pg_class WHERE relname='dolt_ignore'}),'t','ignore table is versioned storage');
fails(q{SELECT name FROM other.dolt_ignore},qr/table not found: other.dolt_ignore/,'unknown schema');
sql(q{CREATE TABLE foo(pk int) USING dolt_prolly_table; CREATE TABLE generated_foo(pk int) USING dolt_prolly_table; CREATE TABLE generated_exception(pk int) USING dolt_prolly_table});
sql(q{SELECT dolt_add('-A')});
is(sql(q{SELECT * FROM dolt_status ORDER BY table_name}),"public.dolt_ignore|t|new table\npublic.foo|t|new table\npublic.generated_exception|t|new table\npublic.generated_foo|f|new table",'add -A skips ignored untracked tables');
is(sql(q{SELECT to_table_name FROM dolt_diff_summary('STAGED','WORKING')}),'public.generated_foo','working set keeps the ignored table');
fails(q{SELECT * FROM dolt_diff('STAGED','WORKING')},qr/requires a from..to revision range/,'two-argument dolt_diff needs a range');
sql(q{SELECT dolt_commit('-m','tracked tables')});
is(sql(q{SELECT * FROM dolt_status}),'public.generated_foo|f|new table','ignored table stays untracked after commit');
fails(q{SELECT dolt_commit('-Am','commit -A')},qr/nothing to commit/,'only ignored tables leave nothing to commit');
sql(q{CREATE TABLE foo2(pk int) USING dolt_prolly_table; SELECT dolt_commit('-Am','commit -A')});
is(sql(q{SELECT * FROM dolt_status}),'public.generated_foo|f|new table','commit -A also respects ignore');
sql(q{SELECT dolt_add('generated_foo'); SELECT dolt_commit('-m','explicit add')});
is(sql(q{SELECT count(*) FROM dolt_status}),'0','explicitly added ignored tables are committed');
# Pattern semantics per schema: more specific patterns win, contradictions keep the table.
sql(q{CREATE SCHEMA newschema; INSERT INTO newschema.dolt_ignore VALUES ('test_*', true), ('generated_exception', true), ('a*', true), ('a?', false), ('b*', true), ('b%', false)});
is(sql(q{SET search_path=newschema; SELECT * FROM dolt_ignore ORDER BY pattern}),"a*|t\na?|f\nb%|f\nb*|t\ngenerated_exception|t\ntest_*|t",'search path selects the schema');
is(sql(q{SELECT * FROM public.dolt_ignore ORDER BY pattern}),"generated_*|t\ngenerated_exception|f",'schemas keep separate patterns');
is(sql(q{SET search_path=newschema; SELECT to_table_name FROM dolt_diff_summary('main','WORKING','dolt_ignore')}),'newschema.dolt_ignore','search path disambiguates table arguments');
sql(q{CREATE TABLE newschema.foo(pk int) USING dolt_prolly_table; CREATE TABLE newschema.test_foo(pk int) USING dolt_prolly_table; CREATE TABLE newschema.generated_foo(pk int) USING dolt_prolly_table; CREATE TABLE newschema.generated_exception(pk int) USING dolt_prolly_table; CREATE TABLE newschema.ab(pk int) USING dolt_prolly_table; CREATE TABLE newschema.abc(pk int) USING dolt_prolly_table; CREATE TABLE newschema.bb(pk int) USING dolt_prolly_table});
sql(q{SELECT dolt_add('-A')});
is(sql(q{SELECT table_name,staged FROM dolt_status ORDER BY table_name}),"newschema|t\nnewschema.ab|t\nnewschema.abc|f\nnewschema.bb|t\nnewschema.dolt_ignore|t\nnewschema.foo|t\nnewschema.generated_exception|f\nnewschema.generated_foo|t\nnewschema.test_foo|f",'per-schema patterns, specificity and contradictions');
sql(q{SELECT dolt_commit('-m','newschema')});
sql(q{SELECT dolt_add('newschema.abc','newschema.generated_exception','newschema.test_foo','public.generated_foo'); SELECT dolt_commit('-m','ignored tables added by name')});
is(sql(q{SELECT count(*) FROM dolt_status}),'0','everything committed');
# ---- dolt_schemas and dolt_procedures are generated from the catalog ----
is(sql(q{SELECT * FROM dolt_schemas}),'','no views yet');
is(sql(q{SELECT * FROM dolt_procedures}),'','no procedures yet');
sql(q{CREATE VIEW myview AS SELECT 2 + 2 AS four; CREATE VIEW newschema.other AS SELECT 1 AS one; CREATE PROCEDURE p1() LANGUAGE sql AS 'SELECT 1'});
is(sql(q{SELECT type,name,fragment,extra::text FROM dolt_schemas}),'view|myview|CREATE VIEW myview AS  SELECT 2 + 2 AS four;|{"CreatedAt":0}','views of the first search path schema');
is(sql(q{SELECT dolt_schemas.name FROM newschema.dolt_schemas}),'other','qualified schema');
is(sql(q{SET search_path=newschema,public; SELECT name FROM dolt_schemas}),'other','first schema on the search path wins');
is(sql(q{SELECT name,create_stmt LIKE 'CREATE OR REPLACE PROCEDURE public.p1()%' FROM dolt_procedures}),'p1|t','procedures of a schema');
fails(q{SELECT name FROM other.dolt_schemas},qr/table not found: other.dolt_schemas/,'unknown schema for dolt_schemas');
fails(q{SELECT name FROM other.dolt_procedures},qr/table not found: other.dolt_procedures/,'unknown schema for dolt_procedures');
sql(q{DROP VIEW myview; DROP VIEW newschema.other; DROP PROCEDURE p1});
is(sql(q{SELECT count(*) FROM dolt_schemas}),'0','dropped views disappear');
# ---- dolt_patch and dolt_schema_diff ----
sql(q{CREATE TABLE t1(pk int PRIMARY KEY, data jsonb, name text) USING dolt_prolly_table; INSERT INTO t1 VALUES (1,'{"text": "hello"}','it''s'); CREATE TABLE t2(pk int PRIMARY KEY, v int, w int) USING dolt_prolly_table; INSERT INTO t2 VALUES (1,1,1),(2,2,2)});
is(sql(q{SELECT statement_order,table_name,diff_type,statement FROM dolt_patch('HEAD','WORKING','t1')}),qq{1|public.t1|schema|CREATE TABLE "t1" (\n  "pk" integer NOT NULL,\n  "data" jsonb,\n  "name" text,\n  PRIMARY KEY ("pk")\n);\n2|public.t1|data|INSERT INTO "t1" ("pk","data","name") VALUES (1,'{\\"text\\": \\"hello\\"}','it\\'s');},'create and insert statements with Dolt quoting');
is(sql(q{SELECT count(*),count(DISTINCT table_name) FROM dolt_patch('HEAD','WORKING')}),'5|2','all changed tables, numbered across tables');
is(sql(q{SELECT from_table_name,to_table_name,from_create_statement,to_create_statement FROM dolt_schema_diff('HEAD','WORKING','t2')}),qq{|public.t2||CREATE TABLE "t2" (\n  "pk" integer NOT NULL,\n  "v" integer,\n  "w" integer,\n  PRIMARY KEY ("pk")\n);},'schema diff of an added table');
is(sql(q{SELECT statement FROM dolt_patch('WORKING','HEAD','t1')}),'DROP TABLE "t1";','reverse patch drops the table');
sql(q{SELECT dolt_commit('-Am','tables')});
sql(q{UPDATE t1 SET data='{"a":1}' WHERE pk=1; INSERT INTO t1 VALUES (2,NULL,'two'); DELETE FROM t2 WHERE pk=2; ALTER TABLE t1 ADD COLUMN c text; ALTER TABLE t2 DROP COLUMN w});
is(sql(q{SELECT statement_order,table_name,diff_type,statement FROM dolt_patch('HEAD','WORKING')}),qq{1|public.t1|schema|ALTER TABLE "t1" ADD COLUMN "c" text;\n2|public.t1|data|UPDATE "t1" SET "data"='{\\"a\\": 1}' WHERE "pk"=1;\n3|public.t1|data|INSERT INTO "t1" ("pk","data","name","c") VALUES (2,NULL,'two',NULL);\n4|public.t2|schema|ALTER TABLE "t2" DROP COLUMN "w";\n5|public.t2|data|DELETE FROM "t2" WHERE "pk"=2;},'schema statements precede data statements');
is(sql(q{SELECT from_commit_hash=dolt_hashof('main'),to_commit_hash FROM dolt_patch('main','WORKING') LIMIT 1}),'t|WORKING','commit hashes and WORKING label');
is(sql(q{SELECT from_table_name,to_table_name,length(from_create_statement)<length(to_create_statement) FROM dolt_schema_diff('HEAD','WORKING') ORDER BY 1}),"public.t1|public.t1|t\npublic.t2|public.t2|f",'schema diff of altered tables');
is(sql(q{SELECT count(*) FROM dolt_schema_diff('HEAD','HEAD')}),'0','identical revisions');
is(sql(q{SELECT table_name,ordinality FROM dolt_patch('HEAD','WORKING','t2') WITH ORDINALITY}),"public.t2|1\npublic.t2|2",'WITH ORDINALITY');
sql(q{SELECT dolt_commit('-Am','changes')});
is(sql(q{SELECT count(*) FROM dolt_patch('HEAD~1','HEAD')}),'5','patch between commits');
fails(q{SELECT * FROM dolt_patch('HEAD','WORKING','nope')},qr/table not found: public.nope/,'unknown table');
fails(q{SELECT * FROM dolt_schema_diff('nope','WORKING')},qr/unknown revision: nope/,'unknown revision');
# ---- administrative procedures ----
is(sql(q{SELECT dolt_gc(),dolt_gc('--shallow'),dolt_gc('--full'),dolt_purge_dropped_databases(),dolt_thread_dump()}),'0|0|0|0|','no-op administration');
fails(q{SELECT dolt_gc('--bogus')},qr/unsupported/,'gc rejects unknown options');
fails(q{SELECT dolt_undrop('db')},qr/cannot be restored/,'undrop is not available');
# Aliases: pg_catalog functions are not public and the dolt_ prefix resolves per schema.
is(sql(q{SELECT count(*) FROM pg_catalog.dolt_patch('HEAD','WORKING')}),'0','qualified function');
is(sql(q{SELECT has_function_privilege('public','pg_catalog.dolt_patch(text,text)','EXECUTE')}),'f','functions are superuser-only');
done_testing();
