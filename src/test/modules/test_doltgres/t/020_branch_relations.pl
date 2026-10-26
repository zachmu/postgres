# Tables on branches are ordinary relations with durable OIDs; revisions are
# read-only relations with leased OIDs. See BRANCH-RELATION-IDENTITY-PLAN.md.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('branch_relations');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}
my $durable=q{BETWEEN 2684354560 AND 3221225471};

# ---- identity ----
sql(q{CREATE TABLE t(id int PRIMARY KEY,v int); INSERT INTO t SELECT g,g*10 FROM generate_series(1,200) g; SELECT dolt_commit('-Am','base'); SELECT dolt_branch('other')});
my $main_oid=sql(q{SELECT 't'::regclass::oid});
my $other_oid=sql(q{SELECT dolt_checkout('other'); SELECT 't'::regclass::oid});
$other_oid=(split /\n/,$other_oid)[-1];
ok($main_oid>=2684354560 && $main_oid<3221225472,'a branch head table has a durable OID');
isnt($main_oid,$other_oid,'each branch has its own OID for the same table');
is(sql(q{SELECT 't'::regclass::oid}),$main_oid,'the OID is the same in every session');
is(sql(q{SELECT count(*) FROM pg_dolt_identity WHERE identbranch='other' AND identname IN ('public.t','public.t_pkey')}),'4','branch creation creates table, index, row type and array type identities');
sql(q{ALTER TABLE t ALTER COLUMN v SET STATISTICS 50});
is(sql(q{SELECT 't'::regclass::oid}),$main_oid,'schema changes keep the OID');

# ---- ordinary access ----
like(sql(q{EXPLAIN (COSTS OFF) SELECT v FROM t WHERE id=7}),qr/Index (Only )?Scan using t_pkey/,'primary-key lookup is an index scan on main');
like(sql(q{EXPLAIN (COSTS OFF) SELECT v FROM "postgres/other".public.t WHERE id=7}),qr/Index (Only )?Scan using t_pkey/,'and on another branch');
is(sql(q{SELECT ctid IS NOT NULL FROM t WHERE id=7}),'t','ctid is available');
is(sql(q{BEGIN; SELECT v FROM t WHERE id=7 FOR UPDATE; COMMIT}),'70','rows lock like any table');
sql(q{SELECT dolt_checkout('other'); UPDATE t SET v=-1 WHERE id=7});
is(sql(q{SELECT a.v,b.v FROM t a JOIN "postgres/other".public.t b USING(id) WHERE id=7}),'70|-1','one query joins two branches of one table');
sql(q{SELECT dolt_checkout('other'); ALTER TABLE t ADD COLUMN note text DEFAULT 'other'});
is(sql(q{SELECT row_to_json(a)::text,row_to_json(b)::text FROM t a JOIN "postgres/other".public.t b USING(id) WHERE id=1}),'{"id":1,"v":10}|{"id":1,"v":10,"note":"other"}','incompatible same-name tables on two branches read in one query');

# ---- revisions ----
my $base=sql(q{SELECT commit_hash FROM dolt_log('-n','1')});
like(sql(qq{EXPLAIN (COSTS OFF) SELECT v FROM t AS OF '$base' WHERE id=7}),qr/Index (Only )?Scan using t_pkey/,'a revision is an ordinary relation with index scans');
cmp_ok(sql(qq{SELECT 't'::regclass::oid < (SELECT c.oid FROM pg_class AS OF '$base' c WHERE relname='t')}),'eq','t','a revision has a leased OID, not a durable one');
fails(qq{SELECT * FROM t AS OF '$base' FOR UPDATE},qr/is a revision, which is read-only/,'revision rows cannot be locked');
my $identities=sql(q{SELECT count(*) FROM pg_dolt_identity});
for my $i (1..10) { sql("UPDATE t SET v=v+1 WHERE id=1; SELECT dolt_commit('-Am','r$i')"); }
my $s=$node->background_psql('postgres');
for my $i (1..10) { $s->query_safe("SELECT v FROM t AS OF 'HEAD~$i' WHERE id=1"); }
$s->quit;
is(sql(q{SELECT count(*) FROM pg_dolt_identity}),$identities,'reading many revisions allocates no durable OIDs');

# ---- invalidation ----
my $plan=$node->background_psql('postgres',on_error_stop=>0);
$plan->query_safe(q{PREPARE other_row AS SELECT * FROM "postgres/other".public.t WHERE id=2});
is($plan->query_safe('EXECUTE other_row'),'2|20|other','prepared plan reads the other branch');
sql(q{SELECT dolt_checkout('other'); ALTER TABLE t DROP COLUMN note});
$plan->query('EXECUTE other_row');
like($plan->{stderr},qr/cached plan must not change result type/,'a schema change on that branch invalidates the prepared statement, as for heap');
$plan->quit;

# ---- locks ----
my $reader=$node->background_psql('postgres');
$reader->query_safe(q{BEGIN; SELECT count(*) FROM t});
is($reader->query_safe(qq{SELECT count(*) FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='relation' AND relation=$main_oid}),'1','readers lock the branch table by its durable OID');
is(sql(q{SET lock_timeout='2s'; SELECT dolt_checkout('other'); ALTER TABLE t ADD COLUMN later int; SELECT 'done'}),qq{(0,"Switched to branch 'other'")\ndone},'DDL on another branch never waits for this branch');
my ($code,$out,$err)=$node->psql('postgres',q{SET lock_timeout='1s'; ALTER TABLE t ADD COLUMN later int});
like($err,qr/lock timeout/,'DDL on the same branch waits for its readers');
$reader->query_safe('COMMIT');$reader->quit;

# ---- identity lifetime ----
sql(q{SELECT dolt_checkout('other'); DROP TABLE t});
is(sql(q{SELECT count(*) FROM pg_dolt_identity WHERE identbranch='other' AND identname='public.t'}),'0','DROP deletes the table identities');
sql(q{SELECT dolt_checkout('other'); CREATE TABLE t(id int PRIMARY KEY)});
isnt(sql(q{SELECT dolt_checkout('other'); SELECT 't'::regclass::oid}),"(0,\"Switched to branch 'other'\")\n$other_oid",'a re-created table has a new OID');
sql(q{SELECT dolt_branch('-D','other')});
is(sql(q{SELECT count(*) FROM pg_dolt_identity WHERE identbranch='other'}),'0','branch deletion deletes its identities');
is(sql("SELECT count(*) FROM pg_dolt_identity WHERE identoid $durable AND identbranch='main'"),'4','main keeps its identities');

# ---- standby ----
# Locks are logged by durable OID: replay conflicts with standby readers of
# the same branch only.
sql(q{SELECT dolt_branch('side')});
$node->backup('relations');
my $standby=PostgreSQL::Test::Cluster->new('relations_standby');
$standby->init_from_backup($node,'relations',has_streaming=>1);
$standby->append_conf('postgresql.conf',"max_standby_streaming_delay=500ms\n");
$standby->start;
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres',q{SELECT 't'::regclass::oid}),$main_oid,'the standby resolves the same durable OID');
my $sreader=$standby->background_psql('postgres',on_error_stop=>0);
$sreader->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t});
sql(q{SELECT dolt_checkout('side'); ALTER TABLE t ADD COLUMN side_only int});
$node->wait_for_catchup($standby);
is($sreader->query_safe(q{SELECT count(*) FROM t}),'200','DDL replayed on another branch does not cancel a standby reader');
sql(q{ALTER TABLE t ADD COLUMN main_only int});
$node->wait_for_catchup($standby);
eval { $sreader->query(q{SELECT count(*) FROM t}); $sreader->quit; };
ok($standby->log_contains(qr/conflict with recovery/),'DDL replayed on the same branch conflicts with a standby reader');
$standby->stop;
$node->stop;
done_testing();
