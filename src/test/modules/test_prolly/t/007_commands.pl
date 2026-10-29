use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('commands');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sub fails {
 my($q,$pat,$label)=@_;my($status,$out,$err)=$node->psql('postgres',$q);
 isnt($status,0,$label);like($err,$pat,"$label diagnostic");
}
is(sql('SELECT message FROM dolt_log()'),'Initialize data repository','repository initialized with the database');
sql(q{CREATE SCHEMA empty_space; CREATE TABLE t(id int PRIMARY KEY,v text COLLATE "C") USING dolt_prolly_table; INSERT INTO t VALUES(1,'one')});
my $first=sql(q{SELECT dolt_commit('-Am','first','--author','Test Author <author@example.com>','--date','2020-01-02 03:04:05+00')});
like($first,qr/^[0-9a-f]{64}$/,'commit returns native SHA256 hash');
is(sql('SELECT count(*) FROM dolt_log()'),'2','repository initialization and first user commit');
is(sql("SELECT message,author,author_email,author_date FROM dolt_log('$first') LIMIT 1"),'first|Test Author|author@example.com|2020-01-02 03:04:05','log metadata matches commit arguments');
is(sql(q{SELECT message FROM dolt_log('HEAD~1')}),'Initialize data repository','repository ancestor');
is(sql(q{SELECT count(*) FROM dolt_log() WHERE message='CREATE DATABASE'}),'0','no storage-library database creation commit');
is(sql(q{SELECT parents IS NOT NULL, signature IS NULL FROM dolt_log('--parents','-n','1')}),'t|t','reference optional log columns');
is(sql("SELECT status FROM dolt_branch('feature','$first')"),'0','branch returns named status column');
is(sql(q{SELECT dolt_tag('v1','HEAD','-m','release','--author','Tagger <tag@example.com>')}),'0','tag creation');
is(sql(q{SELECT tag_name,tag_hash,tagger,email,message FROM dolt_tags}),"v1|$first|Tagger|tag\@example.com|release",'tag view matches reference metadata');
is(sql('SELECT count(*) FROM dolt_log'),'2','log system view');
is(sql(q{SELECT commit_hash FROM dolt_log('v1','-n','1')}),$first,'tag resolves commit');
like(sql(q{SELECT refs FROM dolt_log('-n','1')}),qr/HEAD -> .*feature.*main.*tag: v1/,'reference decoration includes branches and tags');
fails(q{SELECT dolt_branch('feature')},qr/already exists/,'duplicate branch rejected');
fails(q{SELECT dolt_tag('v1')},qr/already exists/,'duplicate tag rejected');
fails(q{SELECT dolt_branch('bad..name')},qr/invalid.*name/,'invalid branch rejected');
fails(q{SELECT dolt_commit('-m','empty')},qr/nothing to commit/,'unstaged commit empty');
is(sql(q{SELECT * FROM dolt_diff_summary('HEAD','WORKING')}),'','clean working tree');
sql(q{UPDATE t SET v='two' WHERE id=1});
is(sql(q{SELECT * FROM dolt_diff_summary('HEAD','WORKING')}),'public.t|public.t|modified|t|f','live data diff summary');
is(sql(q{SELECT * FROM dolt_diff_summary('HEAD','STAGED')}),'','staging not automatic');
is(sql(q{SELECT status FROM dolt_add('.')}),'0','add stages all');
sql(q{UPDATE t SET v='three' WHERE id=1});
my $second=sql(q{SELECT hash FROM dolt_commit('-m','staged version')});
like($second,qr/^[0-9a-f]{64}$/,'commit named hash column');
is(sql(q{SELECT * FROM dolt_diff_summary('HEAD','WORKING','t')}),'public.t|public.t|modified|t|f','unstaged later change survives commit');
is(sql("SELECT * FROM dolt_diff_summary('$first','$second','public.t')"),'public.t|public.t|modified|t|f','historical diff');
my $third=sql(q{SELECT dolt_commit('-A','-m','all current')});
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING')}),'0','all stages current contents');
is(sql("SELECT commit_hash FROM dolt_log('feature','-n','1')"),$first,'branch independent of later main commits');
is(sql(q{SELECT dolt_commit('--skip-empty','-m','skipped')}),'','skip empty returns null');
my $empty=sql(q{SELECT dolt_commit('--allow-empty','-m','empty allowed')});
isnt($empty,$third,'allow empty creates history node');
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD~1','HEAD')}),'0','empty commit has identical state');
fails(q{BEGIN; SELECT dolt_commit('-Am','invalid'); COMMIT},qr/transaction block/,'commit rejects explicit transaction');
is(sql(q{SELECT length(upper(dolt_commit('--allow-empty','-m','wrapped')))}),'64','scalar-wrapped command accepted');
fails(q{SELECT dolt_commit('-Am','invalid') WHERE (SELECT true)},qr/standalone SELECT/,'subquery-wrapped command rejected');
fails(q{SELECT dolt_commit('-Am','invalid') FROM t},qr/standalone SELECT/,'per-row invocation rejected');
fails(q{SELECT dolt_commit('--amend','-m','invalid')},qr/unsupported/,'unsupported flag explicit');
fails(q{SELECT dolt_commit(VARIADIC ARRAY[NULL]::text[])},qr/cannot be null/,'null args rejected');
# Staging participates in an ordinary transaction and rolls back with it.
sql(q{UPDATE t SET v='unstaged' WHERE id=1});
sql(q{BEGIN; SELECT dolt_add('.'); ROLLBACK});
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','STAGED')}),'0','staging rollback');
sql(q{CREATE TABLE heap_only(id int) USING heap});
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING') WHERE to_table_name LIKE '%heap_only'}),'0','native heap tables are unversioned');
sql('DROP TABLE heap_only');
sql(q{CREATE TABLE with_default(id int DEFAULT 4) USING dolt_prolly_table});
sql(q{SELECT dolt_commit('-Am','captured default')});
is(sql(q{SELECT count(*) FROM pg_attrdef WHERE adrelid='with_default'::regclass}),'1','portable default is captured');
sql('DROP TABLE with_default');
# Historical reads retain Repeatable Read views while HEAD advances.
my $reader=$node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $oldhead=$reader->query_safe(q{SELECT commit_hash FROM dolt_log('-n','1')});
sql(q{SELECT dolt_commit('-Am','snapshot advancement')});
is($reader->query_safe(q{SELECT commit_hash FROM dolt_log('-n','1')}),$oldhead,'history read retains snapshot');
$reader->query_safe('COMMIT');$reader->quit;
is(sql(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING'); COMMIT}),'0','live capture reads the working schema under Repeatable Read');
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING','missing')}),'0','an unknown diff table has no changes, as in Dolt');
my $objects=sql('SELECT count(*) FROM pg_dolt_object');
sql(q{SELECT * FROM dolt_diff_summary('HEAD','WORKING')});
is(sql('SELECT count(*) FROM pg_dolt_object'),$objects,'live diff does not persist objects');

# Publication rejects a stale HEAD token after waiting for a competing move.
my $blocker=$node->background_psql('postgres');
my $writer=$node->background_psql('postgres',on_error_stop=>0);
$blocker->query_safe(q{BEGIN; SELECT dolt_manifest_cas('heads/main',token,target) FROM dolt_manifest_read('heads/main')});
my $pid=$writer->query_safe('SELECT pg_backend_pid()');
$writer->query_until(qr/committing/, "\\echo committing\nSELECT dolt_commit('--allow-empty','-m','loser');\n");
ok($node->poll_query_until('postgres',"SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$pid"),'commit waits for concurrent ref move');
$blocker->query_safe('COMMIT');
my($out,$err)=$writer->query('SELECT 1');
is($err,1,'stale history publisher fails instead of overwriting');
$blocker->quit;$writer->quit;
is(sql(q{SELECT count(*) FROM dolt_log() WHERE message='loser'}),'0','failed publisher left no visible history');

# Capture releases its DDL coordination locks after copying immutable state.
my $stager=$node->background_psql('postgres');
$stager->query_safe(q{BEGIN; SELECT dolt_add('.')});
sql(q{SET lock_timeout='1s'; UPDATE t SET v='writer after stage' WHERE id=1});
pass('data writer proceeds while staged transaction remains open');
# Versioned DDL on this branch waits for the staging transaction, which holds
# the branch's working set; unversioned DDL must not.
sql(q{SET lock_timeout='1s'; CREATE TABLE after_capture(id int) USING heap; DROP TABLE after_capture});
pass('unversioned DDL proceeds after capture while staged transaction remains open');
$stager->query_safe('ROLLBACK');$stager->quit;

# Old immutable history survives dropping source storage and crash restart.
sql(q{DROP TABLE t});
is(sql(q{SELECT * FROM dolt_diff_summary('HEAD','WORKING')}),'public.t||dropped|t|t','dropped table diff');
my $dropped=sql(q{SELECT dolt_commit('-Am','drop table')});
$node->stop('immediate');$node->start;
is(sql(q{SELECT commit_hash FROM dolt_log('-n','1')}),$dropped,'committed HEAD recovers');
is(sql("SELECT * FROM dolt_diff_summary('$first','$dropped')"),'public.t||dropped|t|t','history diff survives source drop and restart');
# Reconstructing identical definitions/data yields identical database state.
my $first_state=sql("SELECT encode(objchildren[1],'hex') FROM pg_dolt_object WHERE objhash=decode('$first','hex')");
sql(q{CREATE TABLE t(id int PRIMARY KEY,v text COLLATE "C") USING dolt_prolly_table; INSERT INTO t VALUES(1,'one')});
my $restored=sql(q{SELECT dolt_commit('-Am','restored same contents')});
is(sql("SELECT encode(objchildren[1],'hex') FROM pg_dolt_object WHERE objhash=decode('$restored','hex')"),$first_state,'database root independent of relation OIDs and prior history');
sql('CREATE SCHEMA another_empty');
my $with_namespace=sql(q{SELECT dolt_commit('-Am','empty namespace')});
isnt(sql("SELECT encode(objchildren[1],'hex') FROM pg_dolt_object WHERE objhash=decode('$with_namespace','hex')"),$first_state,'empty namespace participates in database hash');
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD~1','HEAD')}),'0','table diff summary does not invent rows for namespace changes');
sql(q{COMMENT ON TABLE t IS 'description'});
is(sql(q{SELECT data_change,schema_change FROM dolt_diff_summary('HEAD','WORKING','t')}),'f|t','table comment captured as schema metadata');
sql(q{COMMENT ON TABLE t IS NULL});
# Use a connection to verify capture ignores and restores caller formatting.
my $session=$node->background_psql('postgres');
$session->query_safe('SET search_path=public,pg_catalog');
is($session->query_safe(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING')}),'0','removing comment restores committed schema');
$session->query_safe('SET quote_all_identifiers=on');
is($session->query_safe(q{SELECT count(*) FROM dolt_diff_summary('WORKING','WORKING')}),'0','capture with quoted identifiers is stable');
is($session->query_safe(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING')}),'0','caller quoting does not change schema capture');
is($session->query_safe('SHOW quote_all_identifiers'),'on','caller quoting restored');
$session->query_safe(q{SELECT * FROM dolt_diff_summary('HEAD','WORKING')});
is($session->query_safe('SHOW search_path'),'public, pg_catalog','internal search_path restored');
$session->quit;
sql(q{CREATE ROLE reader});
fails(q{SET ROLE reader; SELECT * FROM dolt_log()},qr/permission denied/,'history authorization restricted');
fails(q{SET ROLE reader; SELECT * FROM dolt_tags},qr/permission denied/,'tag view authorization restricted');
$node->stop;
done_testing();
