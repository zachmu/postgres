# User-defined types made in a versioned database are versioned schema
# members: enums, domains and composite types, branched and merged like the
# rest of the schema.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('versioned_types');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}
sql(q{CREATE TABLE seed(id int PRIMARY KEY)});

# ---- enums ----
sql(q{CREATE TYPE mood AS ENUM ('sad','ok','happy')});
sql(q{CREATE TABLE e(id int PRIMARY KEY, m mood DEFAULT 'ok', ms mood[]); INSERT INTO e VALUES (1,'happy','{sad,ok}'),(2,'sad',NULL); INSERT INTO e(id) VALUES (3)});
is(sql(q{SELECT string_agg(id||':'||m, ',' ORDER BY m) FROM e}),'2:sad,3:ok,1:happy','enum columns sort in label order');
is(sql(q{SELECT ms[2] FROM e WHERE id=1}),'ok','enum arrays');
sql(q{ALTER TYPE mood ADD VALUE 'meh' BEFORE 'ok'; ALTER TYPE mood ADD VALUE 'glad' AFTER 'happy'});
is(sql(q{SELECT enum_range(NULL::mood)}),'{sad,meh,ok,happy,glad}','ADD VALUE');
is(sql(q{SELECT m FROM e WHERE id=1}),'happy','stored labels read back after ADD VALUE');
# TODO: rows store labels, so renaming one needs a stable label identity or a rewrite.
fails(q{ALTER TYPE mood RENAME VALUE 'happy' TO 'joyful'},qr/renaming a label of a versioned enum/,'RENAME VALUE is rejected');
is(sql(q{SELECT 'meh'::mood < 'ok'::mood AND 'meh'::mood > 'sad'::mood}),'t','added label sorts in place');
fails(q{ALTER TYPE mood ADD VALUE 'ok'},qr/enum label "ok" already exists/,'native enum validation');

# ---- domains ----
sql(q{CREATE DOMAIN posint AS int NOT NULL DEFAULT 1 CHECK (VALUE > 0); CREATE DOMAIN upbeat AS mood CHECK (VALUE <> 'sad')});
sql(q{CREATE TABLE d(id int PRIMARY KEY, p posint, u upbeat); INSERT INTO d(id, u) VALUES (1,'ok')});
is(sql(q{SELECT p||','||u FROM d}),'1,ok','domain default');
fails(q{INSERT INTO d VALUES (2,-5,'ok')},qr/value for domain posint violates check constraint "posint_check"/,'domain CHECK');
fails(q{INSERT INTO d VALUES (2,5,'sad')},qr/value for domain upbeat violates check constraint "upbeat_check"/,'domain over an enum');
fails(q{ALTER DOMAIN posint ADD CONSTRAINT small CHECK (VALUE < 1)},qr/column "p" of table "d" contains values that violate the new constraint/,'new domain constraint validates existing rows');
sql(q{ALTER DOMAIN posint ADD CONSTRAINT small CHECK (VALUE < 100); ALTER DOMAIN posint SET DEFAULT 7});
fails(q{INSERT INTO d VALUES (3,500,'ok')},qr/violates check constraint "small"/,'added domain constraint');
sql(q{INSERT INTO d(id, u) VALUES (4,'ok')});
is(sql(q{SELECT p FROM d WHERE id=4}),'7','changed domain default');
is(sql(q{SELECT string_agg(pg_get_constraintdef(oid), ' / ' ORDER BY conname) FROM pg_constraint WHERE contypid='posint'::regtype}),'CHECK ((VALUE > 0)) / NOT NULL / CHECK ((VALUE < 100))','domain constraints deparse as native');

# ---- composite types ----
sql(q{CREATE TYPE pair AS (a int, b mood); CREATE TABLE c(id int PRIMARY KEY, p pair, ps pair[]); INSERT INTO c VALUES (1, ROW(1,'glad'), ARRAY[ROW(2,'ok')::pair])});
is(sql(q{SELECT (p).b||','||ps[1].a FROM c}),'glad,2','composite columns and arrays');
sql(q{ALTER TYPE pair RENAME ATTRIBUTE a TO x});
is(sql(q{SELECT (p).x FROM c}),'1','renamed attribute');
sql(q{CREATE TYPE spare AS ENUM ('a'); ALTER TYPE spare RENAME TO extra});
is(sql(q{SELECT enum_range(NULL::extra)}),'{a}','an unused type renames');
# TODO: definitions name the types they use; renaming a used type must rewrite them.
fails(q{ALTER TYPE mood RENAME TO feeling},qr/renaming a versioned type that other objects use/,'renaming a used type is rejected');

# ---- routines and dependencies ----
sql(q{CREATE FUNCTION cheer(f mood) RETURNS mood LANGUAGE sql AS $$SELECT 'glad'::mood$$});
is(sql(q{SELECT cheer('sad')}),'glad','routines take and return versioned types');
fails(q{DROP TYPE pair},qr/column p of table c depends on type pair/,'DROP TYPE is blocked by columns');
fails(q{CREATE TABLE h(id int, f mood) USING heap},qr/cannot make an unversioned object depend on a versioned one/,'a heap table cannot use a versioned type');

# ---- branches and merge ----
sql(q{SELECT dolt_commit('-Am','types'); SELECT dolt_branch('other')});
$node->safe_psql('postgres',q{SELECT dolt_checkout('other'); CREATE TYPE color AS ENUM ('red','blue'); ALTER TYPE mood ADD VALUE 'calm'; CREATE TABLE paint(id int PRIMARY KEY, c color); INSERT INTO paint VALUES (1,'blue'); SELECT dolt_commit('-Am','other types')});
is(sql(q{SELECT count(*) FROM pg_type WHERE typname='color'}),'0','a type made on another branch is not visible');
is(sql(q{SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder) FROM "postgres/other".pg_catalog.pg_enum e JOIN "postgres/other".pg_catalog.pg_type t ON t.oid=e.enumtypid WHERE t.typname='mood'}),'sad,meh,ok,happy,glad,calm','another branch\'s labels are readable');
is(sql(q{SELECT enum_range(NULL::mood)}),'{sad,meh,ok,happy,glad}','this branch keeps its own labels');
is(sql(q{SELECT fast_forward FROM dolt_merge('other')}),'1','a type change merges');
is(sql(q{SELECT enum_range(NULL::mood)::text||','||(SELECT c FROM paint)}),'{sad,meh,ok,happy,glad,calm},blue','merged types and their rows');
$node->safe_psql('postgres',q{SELECT dolt_branch('b1'); SELECT dolt_branch('b2'); SELECT dolt_checkout('b1'); ALTER TYPE mood ADD VALUE 'x1'; SELECT dolt_commit('-Am','b1'); SELECT dolt_checkout('b2'); ALTER TYPE mood ADD VALUE 'x2'; SELECT dolt_commit('-Am','b2')});
sql(q{SELECT dolt_merge('b1')});
fails(q{SELECT dolt_merge('b2')},qr/merge conflict in the definition of type public\.mood/,'a type changed on both branches conflicts');

# ---- drops ----
sql(q{DROP TABLE c, d; DROP TYPE pair; DROP DOMAIN upbeat});
is(sql(q{SELECT count(*) FROM pg_type WHERE typname IN ('pair','_pair','upbeat')}),'0','DROP TYPE and DROP DOMAIN');
is(sql(q{SELECT count(*) FROM pg_type AS OF 'HEAD' WHERE typname='pair'}),'1','history keeps dropped types');
$node->stop;
done_testing();
