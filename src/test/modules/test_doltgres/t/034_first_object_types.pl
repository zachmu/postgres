# User types are canonical from their first definition, with no seed table.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('first_object_types');
$node->init(extra => ['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{BEGIN; CREATE TYPE abandoned AS ENUM ('x'); ROLLBACK});
is(sql(q{SELECT count(*) FROM pg_dolt_ref WHERE refname='views/enabled'}), '0',
   'aborted first type rolls back activation');
sql(q{CREATE SCHEMA types; CREATE TYPE types.mood AS ENUM ('sad','ok','happy')});
sql(q{CREATE DOMAIN types.positive AS int CHECK (VALUE > 0)});
my ($ret, $out, $err) = $node->psql('postgres', q{CREATE DOMAIN types.positive AS int});
isnt($ret, 0, 'duplicate domain is rejected');
like($err, qr/already exists/, 'duplicate domain reports native error');
sql(q{CREATE DOMAIN local_positive AS int CHECK (VALUE > 0)});
($ret, $out, $err) = $node->psql('postgres', q{CREATE DOMAIN local_positive AS int});
isnt($ret, 0, 'unqualified duplicate domain is rejected');
like($err, qr/already exists/, 'search path resolves the canonical creation namespace');
sql(q{CREATE TYPE types.pair AS (i int, s text)});
sql(q{CREATE TYPE types.span AS RANGE (subtype=int4)});
sql(q{CREATE TABLE t(id int PRIMARY KEY, e types.mood, d types.positive, c types.pair, a types.mood[], r types.span)});
sql(q{INSERT INTO t VALUES(1,'happy',2,ROW(3,'four'),ARRAY['sad','ok']::types.mood[],'[1,4)')});
is(sql(q{SELECT e::text,d,(c).i,(c).s,a[2]::text,r::text FROM t}), 'happy|2|3|four|ok|[1,4)',
   'enum, domain, composite, array and range stored without a seed table');
sql(q{SELECT dolt_commit('-Am','typed base')});
sql(q{UPDATE t SET e='sad', d=5, c=ROW(6,'seven'), a=ARRAY['happy']::types.mood[], r='[2,7)'});
is(sql(q{SELECT from_e::text,to_e::text,from_d,to_d,(from_c).s,(to_c).s FROM dolt_diff('HEAD','WORKING','t')}),
   'happy|sad|2|5|four|seven', 'row diff decodes revision-bound user types');
sql(q{SELECT dolt_commit('-Am','typed changes')});
is(sql(q{SELECT e::text,(c).s FROM t AS OF 'HEAD~1'}), 'happy|four', 'retained user-type payloads decode');
is(sql(q{SELECT from_e::text,to_e::text FROM dolt_diff('HEAD~1','HEAD','t')}), 'happy|sad',
   'historical diff resolves both type contexts');
$node->restart;
is(sql(q{SELECT e::text,d,(c).i,a[1]::text,r::text FROM t}), 'sad|5|6|happy|[2,7)',
   'typed values survive restart');
is(sql(q{SELECT e::text FROM t AS OF 'HEAD~1'}), 'happy', 'historical types survive restart');
$node->stop;
done_testing();
