use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('branch_drop');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->start;
sub sql { return $node->safe_psql('postgres',$_[0]); }
sub fails { my($q,$pattern,$label)=@_;my($s,$o,$e)=$node->psql('postgres',$q);isnt($s,0,$label);like($e,$pattern,"$label diagnostic"); }
sql(q{CREATE TABLE t(id int PRIMARY KEY,v text) USING dolt_prolly_table; INSERT INTO t VALUES(1,'old')});
my $old=sql(q{SELECT dolt_commit('-Am','base')});

my $owner=$node->background_psql('postgres');
$owner->query_safe('CREATE TEMP TABLE pg_class(fake int); CREATE TEMP TABLE pg_dolt_object(fake int)');
sql(q{SELECT dolt_branch('other'); SELECT dolt_checkout('other')});
# TODO: DROP ownership checks, once the ownership and ACL design settles who
# owns a versioned table.
sql('BEGIN; DROP TABLE t CASCADE; ROLLBACK');
is(sql('SELECT * FROM t'),'1|old','native CASCADE succeeds and rollback restores branch membership');
sql('BEGIN; DROP TABLE t; ROLLBACK');
is(sql('SELECT * FROM t'),'1|old','full abort restores branch membership');
sql('BEGIN; SAVEPOINT s; DROP TABLE t; ROLLBACK TO s; COMMIT');
is(sql('SELECT * FROM t'),'1|old','savepoint rollback restores membership');
my $reader=$node->background_psql('postgres',on_error_stop=>0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
my $plan=$node->background_psql('postgres',on_error_stop=>0);
$plan->query_safe('PREPARE p AS SELECT * FROM t WHERE false');
$owner->query_safe('DROP TABLE t');
is($owner->query_safe('SELECT current_user=session_user'), 't', 'privileged DROP capture restores caller and resists temporary catalog shadows');
$owner->quit;
# Names resolve against the latest schema, as with catalogs: a snapshot that
# never locked the table does not keep it alive.
my ($gone,$goneerr)=$reader->query('SELECT * FROM t');
like($reader->{stderr},qr/does not exist on branch/,'old SQL snapshot sees committed DROP, as for a native table');
$reader->{stderr}='';$reader->query_safe('ROLLBACK');$reader->quit;
my($out,$err)=$plan->query('EXECUTE p');is($err,1,'prepared empty-result query rebinds after DROP');$plan->quit;
fails('SELECT * FROM t',qr/does not exist on branch/,'unqualified read respects dropped membership');
fails('INSERT INTO t VALUES(2,\'new\')',qr/does not exist on branch/,'write cannot target retained physical object');
fails('COPY t TO STDOUT',qr/does not exist on branch/,'COPY storage path respects branch membership');
fails('SELECT * FROM "postgres/main".public.t',qr/does not exist on branch/,'qualified working read respects DROP');
is(sql('SELECT * FROM "postgres/other".public.t'),'1|old','other branch still reads same physical table');
is(sql("SELECT * FROM t AS OF '$old'"),'1|old','committed history survives branch DROP');
is(sql(q{SELECT to_regclass('t') IS NULL}),'t','regclass respects dropped branch membership');
sql('DROP TABLE IF EXISTS t');
sql(q{SELECT dolt_commit('-am','drop on main')});
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING')}),'0','capture does not resurrect retained physical table');
sql(q{SELECT dolt_checkout('other'); INSERT INTO t VALUES(2,'other'); SELECT dolt_commit('-am','other writes')});
is(sql('SELECT * FROM "postgres/other".public.t ORDER BY id'),"1|old\n2|other",'surviving branch can write and commit');
$node->stop('immediate');$node->start;
fails('SELECT * FROM t',qr/does not exist on branch/,'DROP visibility survives restart');
is(sql('SELECT * FROM "postgres/other".public.t ORDER BY id'),"1|old\n2|other",'other branch survives restart');
is(sql("SELECT * FROM t AS OF '$old'"),'1|old','historical schema and chunks retained after restart');
my $pending=$node->background_psql('postgres');
$pending->query_safe(q{SELECT dolt_checkout('other')});
$pending->query_safe('BEGIN; DROP TABLE t');
$node->stop('immediate');$pending->quit;$node->start;
is(sql('SELECT * FROM "postgres/other".public.t ORDER BY id'),"1|old\n2|other",'crash rolls back uncommitted branch DROP');
is(sql("SELECT * FROM t AS OF '$old'"),'1|old','schema and data history remain reachable after aborted DROP crash');
$node->stop;done_testing();
