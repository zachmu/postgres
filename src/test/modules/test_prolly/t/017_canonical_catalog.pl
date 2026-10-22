# Production canonical schemas feed native descriptors and per-reference catalogs.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('canonical_catalog');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sql(q{CREATE ROLE reader; CREATE TABLE t(id int PRIMARY KEY,v int NOT NULL) USING dolt_prolly_table; INSERT INTO t VALUES(1,7); GRANT SELECT ON t TO reader; COMMENT ON COLUMN t.v IS 'integer revision'});
my $a=sql(q{SELECT dolt_commit('-Am','integer definition')});
sql(q{DROP TABLE t; CREATE TABLE t(id int PRIMARY KEY,v text,note text) USING dolt_prolly_table; INSERT INTO t VALUES(1,'seven','new'); GRANT SELECT ON t TO reader; COMMENT ON COLUMN t.v IS 'text revision'});
my $b=sql(q{SELECT dolt_commit('-Am','text definition')});
is(sql("SELECT a.v,b.v,b.note FROM t AS OF '$a' a JOIN t AS OF '$b' b USING(id)"),'7|seven|new','two incompatible canonical revisions read together');
is(sql("SELECT (a).v,(b).v FROM t AS OF '$a' a JOIN t AS OF '$b' b USING(id)"),'7|seven','native whole-row field lookup honors each revision');
is(sql("SELECT row_to_json(a),row_to_json(b) FROM t AS OF '$a' a JOIN t AS OF '$b' b USING(id)"),'{"id":1,"v":7}|{"id":1,"v":"seven","note":"new"}','native composite output uses canonical row types');
for my $case ([$a,'id:integer,v:integer'],[$b,'id:integer,v:text,note:text'])
{
 my ($revision,$expected)=@$case;
 is(sql("SELECT string_agg(a.attname||':'||format_type(a.atttypid,a.atttypmod),',' ORDER BY a.attnum) FROM pg_catalog.pg_class AS OF '$revision' c JOIN pg_catalog.pg_attribute AS OF '$revision' a ON a.attrelid=c.oid WHERE c.relname='t' AND a.attnum>0"),$expected,'revision catalog join matches canonical columns');
 is(sql("SELECT pg_typeof(x)::oid=c.reltype AND ty.typrelid=c.oid FROM t AS OF '$revision' x JOIN pg_catalog.pg_class AS OF '$revision' c ON c.relname='t' JOIN pg_catalog.pg_type AS OF '$revision' ty ON ty.oid=c.reltype"),'t','executor row type and native catalog share identity');
}
is(sql("SELECT a.oid<>b.oid FROM pg_catalog.pg_class AS OF '$a' a CROSS JOIN pg_catalog.pg_class AS OF '$b' b WHERE a.relname='t' AND b.relname='t'"),'t','incompatible schemas have distinct context-bound handles');
is(sql("SELECT col_description(c.oid,2) FROM pg_catalog.pg_class AS OF '$a' c WHERE c.relname='t'"),'integer revision','native description lookup follows bound object');
is(sql("SELECT col_description(c.oid,2) FROM pg_catalog.pg_class AS OF '$b' c WHERE c.relname='t'"),'text revision','description lookup does not cross revisions');
is(sql("SELECT col_description(c.oid,2) FROM (SELECT 1 AS ord,oid FROM pg_catalog.pg_class AS OF '$a' WHERE relname='t' UNION ALL SELECT 2,oid FROM pg_catalog.pg_class AS OF '$b' WHERE relname='t' UNION ALL SELECT 3,oid FROM pg_catalog.pg_class AS OF '$a' WHERE relname='t') c ORDER BY c.ord"),"integer revision\ntext revision\ninteger revision",'one SQL helper call site alternates revision contexts');
is(sql("SELECT n.nspname,c.relname FROM pg_catalog.pg_class AS OF '$a' c JOIN pg_catalog.pg_namespace AS OF '$a' n ON n.oid=c.relnamespace WHERE c.relname='t'"),'public|t','namespace and relation handles agree');
is(sql("SELECT count(*) FROM pg_catalog.pg_depend AS OF '$a' d JOIN pg_catalog.pg_class AS OF '$a' c ON d.objid=c.oid WHERE c.relname='t' AND d.classid='pg_class'::regclass AND d.refclassid='pg_type'::regclass"),'2','canonical column dependencies point to builtin types');
is(sql("SELECT relname FROM \"postgres\@HEAD~1\".pg_catalog.pg_class WHERE relname='t'"),'t','revision database qualifier binds catalog automatically');
is(sql("SET ROLE reader; BEGIN READ ONLY; SELECT row_to_json(x) FROM t AS OF '$a' x; SELECT pg_current_xact_id_if_assigned() IS NULL; COMMIT"),"{\"id\":1,\"v\":7}\nt",'cold ordinary-reader query binds canonical type without writes');
sql(q{CREATE FUNCTION revision_comments(oid) RETURNS SETOF text LANGUAGE sql STABLE AS 'SELECT description FROM pg_catalog.pg_description WHERE objoid=$1 AND objsubid=2'});
is(sql("SELECT r FROM (SELECT 1 AS ord,oid FROM pg_catalog.pg_class AS OF '$a' WHERE relname='t' UNION ALL SELECT 2,oid FROM pg_catalog.pg_class AS OF '$b' WHERE relname='t') c CROSS JOIN LATERAL revision_comments(c.oid) r ORDER BY c.ord"),"integer revision\ntext revision",'SQL set function retains its bound catalog context');
# A physical replacement must never override a canonical definition.
sql(q{DROP TABLE t; CREATE TABLE t(wrong boolean) USING dolt_prolly_table; INSERT INTO t VALUES(true)});
is(sql("SELECT a.v,b.v FROM t AS OF '$a' a JOIN t AS OF '$b' b USING(id)"),'7|seven','unrelated live physical replacement cannot override canonical schemas');
is(sql("PREPARE retained AS SELECT row_to_json(x) FROM t AS OF '$a' x; EXECUTE retained; DISCARD PLANS; EXECUTE retained"),"{\"id\":1,\"v\":7}\n{\"id\":1,\"v\":7}",'prepared plan and replan retain canonical schema selection');
is(sql("BEGIN; DECLARE old_cursor CURSOR FOR SELECT row_to_json(x) FROM t AS OF '$a' x; SELECT row_to_json(x) FROM t AS OF '$b' x; FETCH ALL old_cursor; COMMIT"),"{\"id\":1,\"v\":\"seven\",\"note\":\"new\"}\n{\"id\":1,\"v\":7}",'cursor remains bound while another revision executes');
is(sql("SELECT c.relname,i.indisprimary,i.indisunique,i.indkey FROM pg_catalog.pg_index AS OF '$a' i JOIN pg_catalog.pg_class AS OF '$a' c ON c.oid=i.indexrelid JOIN pg_catalog.pg_class AS OF '$a' t ON t.oid=i.indrelid WHERE t.relname='t'"),'t_pkey|t|t|1','canonical index ownership and keys use the bound table');
is(sql("SELECT con.conname,pg_get_constraintdef(con.oid) FROM pg_catalog.pg_constraint AS OF '$a' con JOIN pg_catalog.pg_class AS OF '$a' c ON c.oid=con.conrelid WHERE c.relname='t' ORDER BY con.conname"),"t_id_not_null|NOT NULL id\nt_pkey|PRIMARY KEY (id)\nt_v_not_null|NOT NULL v",'native constraint deparser follows canonical bound metadata');
my ($status,$out,$err)=$node->psql('postgres',"SELECT * FROM pg_catalog.pg_proc AS OF '$a'");
isnt($status,0,'unimplemented revision catalog rejects');
like($err,qr/revision-qualified catalog .* is not supported/,'unsupported catalog does not expose live rows');
$node->stop('immediate');
$node->start;
is(sql("BEGIN READ ONLY; SELECT row_to_json(x) FROM t AS OF '$a' x; SELECT col_description(c.oid,2) FROM pg_catalog.pg_class AS OF '$b' c WHERE c.relname='t'; COMMIT"),"{\"id\":1,\"v\":7}\ntext revision",'canonical native descriptors reconstruct after restart without materialization');
$node->stop;
done_testing();
