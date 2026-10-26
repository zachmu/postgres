# Views in a versioned database are versioned schema members: their queries
# are stored as SQL with qualified names and analyzed when first used, in the
# catalogs of the revision they belong to.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('versioned_views');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}

sql(q{
CREATE TABLE t(id int PRIMARY KEY, v int);
INSERT INTO t VALUES (1,5),(2,-1);
CREATE FUNCTION doubled(x int) RETURNS int LANGUAGE sql IMMUTABLE AS $$SELECT x * 2$$;
CREATE VIEW positive AS SELECT id, doubled(v) AS w FROM t WHERE v > 0;
CREATE VIEW named(a, b) AS SELECT w, id FROM positive;
CREATE VIEW checked AS SELECT id, v FROM t WHERE v > 0 WITH CHECK OPTION;
});
is(sql(q{SELECT id||':'||w FROM positive}),'1:10','a view over a versioned table and routine');
is(sql(q{SELECT a||':'||b FROM named}),'10:1','a view over a view, with column names');
is(sql(q{SELECT pg_get_viewdef('positive'::regclass)})," SELECT id,\n    doubled(v) AS w\n   FROM t\n  WHERE (v > 0);",'the definition deparses as native');
like(sql(q{SELECT array_to_string(reloptions, ',') FROM pg_class WHERE relname='checked'}),qr/check_option=cascaded/,'view options');

# ---- updatable views ----
sql(q{INSERT INTO checked VALUES (3,7)});
is(sql(q{SELECT v FROM t WHERE id=3}),'7','an insert through an updatable view');
fails(q{INSERT INTO checked VALUES (4,-7)},qr/new row violates check option for view "checked"/,'WITH CHECK OPTION');

# ---- dependencies ----
fails(q{DROP TABLE t},qr/view positive depends on table t/,'a view keeps its table');
fails(q{ALTER TABLE t DROP COLUMN v},qr/view (public\.)?positive depends on column v of table/,'and the columns it uses');
fails(q{DROP FUNCTION doubled(int)},qr/view positive depends on function doubled/,'and the routines it calls');
fails(q{DROP VIEW positive},qr/view named depends on view positive/,'and the views it feeds');

# ---- renames ----
sql(q{ALTER TABLE t RENAME TO base; ALTER TABLE base RENAME COLUMN v TO val; ALTER FUNCTION doubled(int) RENAME TO twice; ALTER VIEW positive RENAME TO pos; ALTER VIEW pos RENAME COLUMN w TO ww});
is(sql(q{SELECT pg_get_viewdef('pos'::regclass)})," SELECT id,\n    twice(val) AS ww\n   FROM base\n  WHERE (val > 0);",'renames of what a view uses rewrite its query');
is(sql(q{SELECT pg_get_viewdef('named'::regclass)})," SELECT ww AS a,\n    id AS b\n   FROM pos;",'and the views that use it');
is(sql(q{SELECT string_agg(a||':'||b, ',' ORDER BY b) FROM named}),'10:1,14:3','renamed views read');

# ---- branches and history ----
sql(q{SELECT dolt_commit('-Am','views'); SELECT dolt_branch('other')});
sql(q{UPDATE base SET val = 6 WHERE id = 1; CREATE OR REPLACE VIEW pos AS SELECT id, twice(twice(val)) AS ww FROM base WHERE val > 0});
is(sql(q{SELECT ww FROM pos WHERE id=1}),'24','CREATE OR REPLACE VIEW');
is(sql(q{SELECT ww FROM pos AS OF 'HEAD' WHERE id=1}),'10','AS OF reads a view as its revision defines it, over its revision\'s rows');
is(sql(q{SELECT a FROM "postgres/other".public.named WHERE b=1}),'10','another branch\'s view');
is(sql(q{SELECT count(*) FROM pg_views WHERE viewname IN ('pos','named','checked')}),'3','pg_views lists versioned views');

# ---- drops ----
sql(q{DROP VIEW named; DROP TABLE base CASCADE});
is(sql(q{SELECT count(*) FROM pg_views WHERE viewname IN ('pos','named','checked')}),'0','DROP VIEW and DROP TABLE CASCADE');
is(sql(q{SELECT count(*) FROM pos AS OF 'HEAD'}),'2','history keeps dropped views');

# Materialized views are tables whose definitions also hold their queries.
sql(q{CREATE TABLE src(a int PRIMARY KEY, b text); INSERT INTO src VALUES (1,'x'),(2,'y');
	CREATE MATERIALIZED VIEW mv AS SELECT a, b FROM src WHERE a > 0;
	CREATE MATERIALIZED VIEW lazy AS SELECT 1 AS one WITH NO DATA});
is(sql(q{SELECT count(*) FROM mv}),'2','a materialized view holds its rows');
fails(q{SELECT * FROM lazy},qr/has not been populated/,'WITH NO DATA');
sql(q{INSERT INTO src VALUES (3,'z'); REFRESH MATERIALIZED VIEW mv; REFRESH MATERIALIZED VIEW lazy});
is(sql(q{SELECT count(*) FROM mv}).':'.sql(q{SELECT one FROM lazy}),'3:1','REFRESH MATERIALIZED VIEW');
sql(q{ALTER TABLE src RENAME TO src2; ALTER TABLE src2 RENAME COLUMN b TO bb; ALTER MATERIALIZED VIEW mv RENAME TO mv2});
is(sql(q{SELECT pg_get_viewdef('mv2'::regclass)})," SELECT a,\n    bb AS b\n   FROM src2\n  WHERE (a > 0);",'renames rewrite a materialized view\'s query');
sql(q{SELECT dolt_commit('-Am','materialized')});
is(sql(q{SELECT dolt_checkout('-b','refreshed'); INSERT INTO src2 VALUES (4,'w'); REFRESH MATERIALIZED VIEW mv2;
	SELECT count(*) FROM mv2; SELECT dolt_checkout('main'); SELECT count(*) FROM mv2}),
	"(0,\"Switched to branch 'refreshed'\")\n4\n(0,\"Switched to branch 'main'\")\n3",'a refresh on a branch stays on it');
sql(q{DROP MATERIALIZED VIEW lazy; DROP TABLE src2 CASCADE});
is(sql(q{SELECT count(*) FROM pg_matviews}),'0','DROP MATERIALIZED VIEW and DROP TABLE CASCADE');
is(sql(q{SELECT count(*) FROM mv2 AS OF 'HEAD'}),'3','history keeps materialized views');

# ---- not versioned ----
is(sql(q{CREATE TEMP VIEW tv AS SELECT 1 AS one; SELECT one FROM tv}),'1','temporary views stay native');
$node->stop;
done_testing();
