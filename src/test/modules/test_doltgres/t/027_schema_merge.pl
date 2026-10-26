# A table changed on both branches merges column by column, as in Dolt, with
# columns, constraints and indexes matched by name. Definitions that do not
# merge are schema conflicts, recorded in dolt_schema_conflicts.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('schema_merge');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
my $db;
sub scenario {$db=$_[0];$node->safe_psql('postgres',"CREATE DATABASE $db");}
sub sql {return $node->safe_psql($db,$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql($db,$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}
# Base commit, then |theirs| on branch other and |ours| on main.
sub branches {my($base,$theirs,$ours)=@_;sql($base."; SELECT dolt_commit('-Am','base'); SELECT dolt_branch('other')");sql("SELECT dolt_checkout('other'); $theirs; SELECT dolt_commit('-Am','theirs')");sql("$ours; SELECT dolt_commit('-Am','ours')");}

# ---- columns ----
scenario('added');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int, b text); INSERT INTO t VALUES (1,10,'x'),(2,20,'y')},
		 q{ALTER TABLE t ADD COLUMN c int DEFAULT 7; INSERT INTO t VALUES (3,30,'z',33); UPDATE t SET a=11 WHERE pk=1},
		 q{INSERT INTO t VALUES (4,40,'w'); UPDATE t SET b='yy' WHERE pk=2});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'0','a column added on one side merges with rows changed on both');
is(sql(q{SELECT string_agg(pk||':'||a||':'||b||':'||c, ',' ORDER BY pk) FROM t}),'1:11:x:7,2:20:yy:7,3:30:z:33,4:40:w:7','our new row takes the added column\'s default');

scenario('reshaped');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int, b text, c int); INSERT INTO t VALUES (1,10,'x',1),(2,20,'y',2)},
		 q{ALTER TABLE t DROP COLUMN c; ALTER TABLE t ADD COLUMN d text DEFAULT 'dd'; UPDATE t SET a=11 WHERE pk=1},
		 q{ALTER TABLE t ADD COLUMN e int; ALTER TABLE t ALTER COLUMN b TYPE varchar(10); INSERT INTO t VALUES (3,30,'z',3,99)});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'0','columns added, dropped and retyped on either side merge');
is(sql(q{SELECT string_agg(attname||' '||format_type(atttypid,atttypmod), ', ' ORDER BY attnum) FROM pg_attribute WHERE attrelid='t'::regclass AND attnum>0}),
   'pk integer, a integer, b character varying(10), e integer, d text','the merged columns: ours in order, then theirs');
is(sql(q{SELECT string_agg(pk||':'||a||':'||b||':'||coalesce(e::text,'-')||':'||d, ',' ORDER BY pk) FROM t}),'1:11:x:-:dd,2:20:y:-:dd,3:30:z:99:dd','rows follow the merged columns');

scenario('widened');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int); INSERT INTO t VALUES (1,1),(2,2)},
		 q{ALTER TABLE t ALTER COLUMN a TYPE bigint; UPDATE t SET a=3000000000 WHERE pk=1},
		 q{UPDATE t SET a=20 WHERE pk=2; INSERT INTO t VALUES (3,3)});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'0','a type changed on one side merges');
is(sql(q{SELECT string_agg(pk||':'||a||':'||pg_typeof(a), ',' ORDER BY pk) FROM t}),'1:3000000000:bigint,2:20:bigint,3:3:bigint','the other side\'s values are converted');

scenario('merged_column');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int); INSERT INTO t VALUES (1,10)},
		 q{ALTER TABLE t ALTER COLUMN a SET DEFAULT 5},
		 q{ALTER TABLE t ALTER COLUMN a SET NOT NULL; INSERT INTO t VALUES (2,2)});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'0','different attributes of one column changed on each side merge');
is(sql(q{SELECT pg_get_expr(adbin,adrelid)||':'||(SELECT attnotnull FROM pg_attribute WHERE attrelid='t'::regclass AND attname='a') FROM pg_attrdef WHERE adrelid='t'::regclass}),'5:true','the column has both changes');

scenario('dropped_changed');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int, d int); INSERT INTO t VALUES (1,10,100),(2,20,200)},
		 q{UPDATE t SET d=101 WHERE pk=1; UPDATE t SET a=21 WHERE pk=2},
		 q{ALTER TABLE t DROP COLUMN d});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'1','a column dropped on one side and changed in a row on the other conflicts, as in Dolt');
is(sql(q{SELECT base_pk||':'||our_diff_type||':'||their_diff_type FROM dolt_conflicts_t}).';'.sql(q{SELECT a FROM t WHERE pk=2}),'1:modified:modified;21','the row conflicts; others merge');
sql(q{SELECT dolt_conflicts_resolve('--ours','t'); SELECT dolt_commit('-Am','resolved')});
is(sql(q{SELECT string_agg(pk||':'||a, ',' ORDER BY pk) FROM t}),'1:10,2:21','the conflict resolves');

scenario('keyless');
branches(q{CREATE TABLE k(a int, b text); INSERT INTO k VALUES (1,'x'),(2,'y')},
		 q{ALTER TABLE k ADD COLUMN c int DEFAULT 0; INSERT INTO k VALUES (3,'z',9)},
		 q{INSERT INTO k VALUES (4,'w'); DELETE FROM k WHERE a=1});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'0','a keyless table merges across an added column');
is(sql(q{SELECT string_agg(a||b||c, ',' ORDER BY a) FROM k}),'2y0,3z9,4w0','keyless rows take the added column\'s default');

# ---- constraints and indexes ----
scenario('violations');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int); INSERT INTO t VALUES (1,10)},
		 q{INSERT INTO t VALUES (2,NULL),(3,-5)},
		 q{ALTER TABLE t ALTER COLUMN a SET NOT NULL; ALTER TABLE t ADD CONSTRAINT pos CHECK (a > 0)});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'1','constraints added on one side report the other side\'s violations');
is(sql(q{SELECT string_agg(violation_type||':'||pk, ',' ORDER BY pk) FROM dolt_constraint_violations_t}),'not null:2,check constraint:3','NOT NULL and CHECK violations');
is(sql(q{SELECT string_agg(pk::text, ',' ORDER BY pk) FROM t}),'1,3','a NOT NULL violation\'s row is left out, as in Dolt');

scenario('indexes');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int, b int); INSERT INTO t VALUES (1,1,1)},
		 q{CREATE INDEX t_b ON t(b); CREATE INDEX t_a ON t(a); INSERT INTO t VALUES (2,2,2)},
		 q{ALTER TABLE t DROP COLUMN b; INSERT INTO t VALUES (3,3)});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}),'0','indexes added on one side merge');
is(sql(q{SELECT string_agg(relname, ',' ORDER BY relname) FROM pg_class WHERE relname IN ('t_a','t_b','t_pkey')}),'t_a,t_pkey','an index over a column the other side dropped is dropped, as in Dolt');
is(sql(q{SET enable_seqscan=off; SELECT pk FROM t WHERE a=3}),'3','a merged index has every merged row');

# ---- schema conflicts ----
scenario('conflicting');
branches(q{CREATE TABLE test(pk int PRIMARY KEY, c0 varchar(20)); INSERT INTO test VALUES (1,'2020-01-01'); CREATE TABLE other_t(x int)},
		 q{ALTER TABLE test ALTER COLUMN c0 TYPE int USING length(c0); INSERT INTO other_t VALUES (1)},
		 q{ALTER TABLE test ALTER COLUMN c0 TYPE date USING c0::date});
is(sql(q{SELECT conflicts, message FROM dolt_merge('other')}),'1|conflicts found','column types changed differently on both sides conflict');
is(sql(q{SELECT table_name||': '||description FROM dolt_schema_conflicts}),"public.test: incompatible column types for column 'c0'",'dolt_schema_conflicts describes it');
is(sql(q{SELECT our_schema FROM dolt.schema_conflicts}),"CREATE TABLE public.test (\n  pk integer NOT NULL,\n  c0 date,\n  CONSTRAINT test_pkey PRIMARY KEY (pk)\n);\nCREATE UNIQUE INDEX test_pkey ON public.test USING dolt_prolly_index (pk);",'with each side\'s CREATE TABLE');
is(sql(q{SELECT "table"||':'||num_conflicts FROM dolt_conflicts}).';'.sql(q{SELECT table_name||':'||status FROM dolt_status WHERE status='schema conflict'}),'test:0;public.test:schema conflict','dolt_conflicts and dolt_status list it, as in Dolt');
is(sql(q{SELECT count(*) FROM other_t}),'1','other tables still merge');
fails(q{SELECT dolt_commit('-Am','conflicted')},qr/unresolved merge conflicts/,'a schema conflict blocks commit');
sql(q{SELECT dolt_conflicts_resolve('--theirs','test'); SELECT dolt_commit('-Am','resolved')});
is(sql(q{SELECT format_type(atttypid,atttypmod)||':'||(SELECT c0 FROM test) FROM pg_attribute WHERE attrelid='test'::regclass AND attname='c0'}),'integer:10','resolving --theirs takes their whole table');

scenario('added_twice');
branches(q{CREATE TABLE t1(pk int PRIMARY KEY, c1 int); INSERT INTO t1 VALUES (1,10)},
		 q{ALTER TABLE t1 ADD COLUMN c2 varchar(50)},
		 q{ALTER TABLE t1 ADD COLUMN c2 int});
is(sql(q{SELECT * FROM dolt_preview_merge_conflicts_summary('main','other')}),'public.t1||1','the preview counts a schema conflict');
fails(q{SELECT * FROM dolt_preview_merge_conflicts('main','other','t1')},qr/schema conflicts found: 1/,'and will not preview its rows');
is(sql(q{SELECT conflicts FROM dolt_merge('other')}).':'.sql(q{SELECT description FROM dolt_schema_conflicts}),'1:different column definitions for our column c2 and their column c2','a column added differently on both sides conflicts');
sql(q{SELECT dolt_merge('--abort')});
is(sql(q{SELECT count(*) FROM dolt_schema_conflicts}),'0','aborting the merge clears it');

scenario('deleted');
branches(q{CREATE TABLE t(pk int PRIMARY KEY, a int); INSERT INTO t VALUES (1,10)},
		 q{DROP TABLE t},
		 q{UPDATE t SET a=11});
is(sql(q{SELECT conflicts FROM dolt_merge('other')}).':'.sql(q{SELECT their_schema||':'||description FROM dolt_schema_conflicts}),
   '1:<deleted>:cannot merge a table deletion with data or schema modification','a table dropped on one side and changed on the other conflicts');

scenario('new_not_null');
branches(q{CREATE TABLE t(pk int PRIMARY KEY); INSERT INTO t VALUES (1)},
		 q{DELETE FROM t; ALTER TABLE t ADD COLUMN n int NOT NULL},
		 q{INSERT INTO t VALUES (2)});
fails(q{SELECT dolt_merge('other')},qr/Unable to merge new column n in table public\.t because it is not-nullable and has no default value/,'a new NOT NULL column without a default cannot merge, as in Dolt');
$node->stop;
done_testing();
