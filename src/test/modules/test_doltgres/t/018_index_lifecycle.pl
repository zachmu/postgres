use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('index_lifecycle');
$node->init(extra=>['--encoding=UTF8','--locale=C']);$node->start;
my $s=$node->background_psql('postgres',on_error_stop=>0);
sub sql { $s->query_safe($_[0]); }
sub rejects {my($out,$failed)=$s->query($_[0]);is($failed,1,$_[1]);diag($s->{stderr}) unless $failed;$s->{stderr}='';}
sql(q{SET statement_timeout='10s'; SET client_min_messages=warning;
CREATE TABLE t(id int PRIMARY KEY,x int,v text) USING dolt_prolly_table;
INSERT INTO t VALUES(1,10,'a'),(2,20,'b');
SELECT dolt_commit('-Am','base'); SELECT dolt_checkout('-b','other')});
sql(q{CREATE UNIQUE INDEX x_idx ON t(x)});
is(sql(q{SELECT indexname FROM pg_indexes WHERE tablename='t' ORDER BY indexname}),"t_pkey\nx_idx",'independent index creation publishes canonical index');
rejects(q{INSERT INTO t VALUES(3,10,'duplicate')},'new unique index validates writes');
sql(q{ALTER TABLE t ALTER COLUMN x TYPE bigint USING x::bigint+1});
is(sql(q{SET enable_seqscan=off; SELECT x,pg_typeof(x) FROM t WHERE x=11}),'11|bigint','indexed TYPE rebuild uses rewritten rows');
sql(q{ALTER TABLE t DROP COLUMN id});
is(sql(q{SELECT * FROM t ORDER BY x}),"11|a\n21|b",'indexed DROP compacts rows');
is(sql(q{SELECT indexname FROM pg_indexes WHERE tablename='t' ORDER BY indexname}),'x_idx','owned primary index deleted with column');
sql(q{ALTER TABLE t ADD CONSTRAINT v_unique UNIQUE(v)});
rejects(q{INSERT INTO t VALUES(30,'a')},'native unique constraint creates a published index');
sql(q{SELECT dolt_checkout('main')});
is(sql(q{SELECT * FROM t ORDER BY id}),"1|10|a\n2|20|b",'other branch schema and rows unaffected');
$s->quit;$node->stop;done_testing();
