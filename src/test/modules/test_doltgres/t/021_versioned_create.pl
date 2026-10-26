# CREATE TABLE of a versioned table runs natively against a provisional copy of
# the branch's catalogs; the result is published to the branch's schema.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('versioned_create');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}

# ---- defaults, constraints and indexes ----
sql(q{CREATE TABLE d(id int PRIMARY KEY, v int DEFAULT 7, w text NOT NULL DEFAULT 'x', at timestamptz DEFAULT now(), CHECK (v > 0))});
sql(q{INSERT INTO d(id) VALUES (1)});
is(sql(q{SELECT id,v,w,at IS NOT NULL FROM d}),'1|7|x|t','column defaults apply');
fails(q{INSERT INTO d VALUES (2,-1,'y')},qr/violates check constraint "d_v_check"/,'CHECK constraint is enforced');
sql(q{CREATE TABLE c(id int, x int, y text, CONSTRAINT cpk PRIMARY KEY (id), CONSTRAINT xpos CHECK (x >= 0), UNIQUE (x, y)) WITH (fillfactor=70)});
sql(q{CREATE INDEX c_y ON c(y DESC NULLS LAST)});
is(sql(q{SELECT string_agg(indexname,',' ORDER BY indexname) FROM pg_indexes WHERE tablename='c'}),'c_x_y_key,c_y,cpk','named, composite unique and non-unique indexes');
is(sql(q{SELECT reloptions IS NULL FROM pg_class WHERE relname='c'}),'t','storage options are ignored');
fails(q{INSERT INTO c VALUES (1,-1,'a')},qr/xpos/,'named CHECK constraint is enforced');
sql(q{CREATE TABLE lk (LIKE d INCLUDING ALL)});
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid='lk'::regclass AND contype IN ('c','p')}),'2','LIKE INCLUDING ALL copies constraints');

# ---- generated columns ----
sql(q{CREATE TABLE g(id int PRIMARY KEY, a int, s int GENERATED ALWAYS AS (a * 2) STORED, v int GENERATED ALWAYS AS (a + 1) VIRTUAL)});
sql(q{INSERT INTO g(id,a) VALUES (1,10)});
is(sql(q{SELECT s,v FROM g}),'20|11','stored and virtual generated columns compute');
fails(q{ALTER TABLE g DROP COLUMN a},qr/column s of table .*g depends on column a/,'generation inputs cannot be dropped alone, as for heap');

# ---- sequences are global ----
sql(q{CREATE TABLE s(id serial PRIMARY KEY, name text); CREATE TABLE i(id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY, name text)});
sql(q{INSERT INTO s(name) VALUES ('one'); INSERT INTO i(name) VALUES ('one')});
sql(q{SELECT dolt_commit('-Am','base'); SELECT dolt_branch('other')});
sql(q{SELECT dolt_checkout('other'); INSERT INTO s(name) VALUES ('other'); INSERT INTO i(name) VALUES ('other')});
sql(q{INSERT INTO s(name) VALUES ('main'); INSERT INTO i(name) VALUES ('main')});
is(sql(q{SELECT string_agg(id::text,',' ORDER BY id) FROM s}),'1,3','serial draws from one sequence shared by every branch');
is(sql(q{SELECT string_agg(id::text,',' ORDER BY id) FROM i}),'1,3','identity draws from one sequence shared by every branch');
fails(q{INSERT INTO i VALUES (9,'x')},qr/GENERATED ALWAYS/,'identity GENERATED ALWAYS is enforced');
sql(q{DROP TABLE s});
is(sql(q{SELECT nextval('s_id_seq')}),'4','dropping a versioned table keeps its global sequence');

# ---- definitions are versioned ----
sql(q{SELECT dolt_checkout('other'); ALTER TABLE d ALTER COLUMN v SET DEFAULT 8});
is(sql(q{SELECT pg_get_expr(adbin,adrelid) FROM pg_attrdef WHERE adrelid='d'::regclass AND adnum=2}),'7','main keeps its own default');
is(sql(q{SELECT pg_get_expr(a.adbin,a.adrelid) FROM "postgres/other".pg_catalog.pg_attrdef a JOIN "postgres/other".pg_catalog.pg_class r ON r.oid=a.adrelid WHERE r.relname='d' AND a.adnum=2}),'8','the other branch sees its changed default');
is(sql(q{SELECT s,v FROM g AS OF 'HEAD'}),'20|11','generated columns read in revisions');

# ---- schemas created before the first versioned table ----
my $fresh='fresh';
$node->safe_psql('postgres',"CREATE DATABASE $fresh");
is($node->safe_psql($fresh,q{SET default_table_access_method=dolt_prolly_table; CREATE SCHEMA early; CREATE TABLE early.t(v int DEFAULT 3); INSERT INTO early.t DEFAULT VALUES; SELECT v FROM early.t}),'3','a schema made before the first versioned table joins the branch with it');

# ---- still unsupported ----
fails(q{CREATE TABLE p(id int) PARTITION BY RANGE (id)},qr/partitioned and inherited versioned tables/,'partitioning is rejected');
fails(q{CREATE UNLOGGED TABLE u(id int)},qr/unlogged versioned tables/,'unlogged is rejected');
fails(q{CREATE TYPE pair AS (a int, b int); CREATE TABLE ct(v pair)},qr/user-defined type codec/,'composite columns are rejected by the codec, not the catalog lookup');
fails(q{CREATE TABLE ctas AS SELECT 1 AS a},qr/prolly tables can only be created with CREATE TABLE/,'CREATE TABLE AS is rejected');
$node->stop;
done_testing();
