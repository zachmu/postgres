use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('index_types');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
my $seq = 'SET enable_indexscan=off; SET enable_indexonlyscan=off; SET enable_bitmapscan=off; ';
my $idx = 'SET enable_seqscan=off; SET enable_bitmapscan=off; ';
sql(q{CREATE TYPE pair AS (n numeric, s text);
CREATE DOMAIN object_number AS oid;
CREATE TYPE object_span AS RANGE (subtype=oid)});
my @cases = (
 ['oid', 'oid', ["'0'", "'1'", "'23'", "'2147483647'", "'2147483648'", "'4294967295'"]],
 ['domain', 'object_number', ["'0'", "'23'", "'4294967295'"]],
 ['bit', 'bit(5)', ["B'00000'", "B'00001'", "B'10000'", "B'10100'", "B'11111'"]],
 ['varbit', 'varbit', ["B''", "B'0'", "B'00'", "B'000000000'", "B'01'", "B'1'", "B'10'", "B'100000001'", "B'11'"]],
 ['inet', 'inet', ["'0.0.0.0/0'", "'10.9.8.7/8'", "'10.0.0.0/16'", "'10.9.8.7/32'", "'11.0.0.0/8'", "'::/0'", "'::1'", "'2001:db8::1/32'", "'2001:db8::/64'"]],
 ['cidr', 'cidr', ["'0.0.0.0/0'", "'10.0.0.0/8'", "'10.0.0.0/16'", "'11.0.0.0/8'", "'::/0'", "'::1/128'"]],
 ['jsonb', 'jsonb', [q{'[]'}, q{'null'}, q{'""'}, q{'"a"'}, q{'"aa"'}, q{'"b"'}, q{'-10'}, q{'1.00'}, q{'true'}, q{'false'}, q{'[null]'}, q{'[[]]'}, q{'[1,2]'}, q{'[1,3]'}, q{'[[1],{}]'}, q{'{}'}, q{'{"a":1}'}, q{'{"b":0}'}, q{'{"aa":1}'}, q{'{"a":1,"bb":[2,3]}'}]],
 ['record', 'pair', [q{'(,)'}, q{'(1,)'}, q{'(-1,a)'}, q{'(1,a)'}, q{'(1,aa)'}, q{'(1,b)'}, q{'(2,a)'}]],
 ['range', 'numrange', [q{'empty'}, q{'(,)'}, q{'(,1)'}, q{'(,1]'}, q{'[0,1)'}, q{'(0,1)'}, q{'[0,1]'}, q{'[1,2)'}, q{'(2,)'}]],
 ['oidrange', 'object_span', [q{'empty'}, q{'(,)'}, q{'[0,1)'}, q{'[23,2147483648)'}, q{'[2147483648,4294967295]'}]],
 ['multirange', 'int4multirange', [q{'{}'}, q{'{(,)}'}, q{'{[0,1)}'}, q{'{[0,1),[3,4)}'}, q{'{[0,1),[4,5)}'}, q{'{[1,2)}'}]],
 ['oidarray', 'oid[]', [q{'{}'}, q{'{0}'}, q{'{23,1}'}, q{'{23,NULL}'}, q{'{2147483648}'}, q{'{4294967295}'}]],
 ['jsonarray', 'jsonb[]', [q{ARRAY['1'::jsonb]}, q{ARRAY['[]'::jsonb]}, q{ARRAY['{}'::jsonb]}, q{ARRAY['null'::jsonb]}]],
 ['recordarray', 'pair[]', [q{ARRAY['(1,a)'::pair]}, q{ARRAY['(1,b)'::pair]}, q{ARRAY['(,)'::pair]}]],
);
for my $case (@cases)
{
 my ($name, $type, $values) = @$case;
 sql("CREATE TABLE t_$name(id int, v $type)");
 my $n = 0;
 sql("INSERT INTO t_$name VALUES ".join(',', map { '(' . ++$n . ",($_)::$type)" } @$values).",(".++$n.",NULL)");
 sql("CREATE INDEX i_$name ON t_$name(v) INCLUDE(id)");
 for my $direction ('ASC NULLS LAST', 'DESC NULLS FIRST')
 {
  my $q = "SELECT v::text AS value FROM t_$name ORDER BY v $direction";
  my $expected = sql($seq.$q);
  is(sql($idx.$q), $expected, "$name ordered scan matches native sort $direction");
  my $plan = sql($idx."EXPLAIN (COSTS OFF) $q");
  like($plan, qr/Index (Only )?Scan/, "$name uses index");
  unlike($plan, qr/Sort/, "$name uses stored order without sorting");
 }
 for my $op ('<','<=','=','>=','>')
 {
  # Every input becomes a boundary, covering equal prefixes and endpoints.
  for my $v (@$values)
  {
   my $q = "SELECT id FROM t_$name WHERE v $op ($v)::$type ORDER BY id";
   is(sql($idx.$q), sql($seq.$q), "$name $op $v matches native predicate");
  }
 }
 sql("DROP INDEX i_$name; CREATE INDEX i_$name ON t_$name(v DESC NULLS LAST)");
 is(sql($idx."SELECT v::text AS value FROM t_$name ORDER BY v DESC NULLS LAST"),
    sql($seq."SELECT v::text AS value FROM t_$name ORDER BY v DESC NULLS LAST"), "$name descending physical key encoding");
 sql("UPDATE t_$name SET v=NULL WHERE id=1; DELETE FROM t_$name WHERE id=2; INSERT INTO t_$name VALUES(10000,($values->[0])::$type)");
 my $changed = "SELECT id FROM t_$name WHERE v=($values->[0])::$type ORDER BY id";
 is(sql($idx.$changed), sql($seq.$changed), "$name incremental insert/update/delete maintains keys");
}
for my $case (
 ['oid','oid',q{'4294967295'},q{'4294967295'}],
 ['jsonb','jsonb',q{'{"a":1.0,"b":2}'},q{'{"b":2,"a":1.00}'}],
 ['record','pair',q{'(1.0,a)'},q{'(1.00,a)'}],
 ['range','numrange',q{'[1.0,2)'},q{'[1.00,2.0)'}],
 ['inet','inet',q{'2001:db8::1'},q{'2001:0db8:0:0:0:0:0:1/128'}])
{
 my ($name,$type,$a,$b)=@$case;
 sql("CREATE TABLE u_$name(v $type UNIQUE); INSERT INTO u_$name VALUES ($a)");
 my ($status,$out,$err)=$node->psql('postgres', "INSERT INTO u_$name VALUES ($b)");
 isnt($status,0,"$name equivalent values violate uniqueness");
 like($err,qr/duplicate key/,"$name native uniqueness diagnostic");
}
# User-provided OIDs are data. Incidental array/composite type OIDs are stripped.
sql(q{CREATE TYPE wrapped_a AS (v oid[]); CREATE TYPE wrapped_b AS (v oid[]);
CREATE TABLE a(v wrapped_a); CREATE TABLE b(v wrapped_b);
INSERT INTO a VALUES (ROW(ARRAY[23,4294967295]::oid[]));
INSERT INTO b VALUES (ROW(ARRAY[23,4294967295]::oid[]))});
is(sql(q{SELECT dolt_prolly_root('a')=dolt_prolly_root('b')}),'t','incidental composite type identity is absent from content');
sql(q{UPDATE b SET v=ROW(ARRAY[24,4294967295]::oid[])});
is(sql(q{SELECT dolt_prolly_root('a')<>dolt_prolly_root('b')}),'t','explicit OID payload affects content');
sql(q{SELECT dolt_commit('-Am','indexed types')});
sql(q{UPDATE t_oid SET v=42 WHERE id=1});
is(sql($idx.q{SELECT v FROM t_oid AS OF 'HEAD' WHERE v=0::oid}),'0','retained OID index resolves original value');
$node->restart;
is(sql($idx.q{SELECT v FROM t_oid WHERE v=4294967295::oid}),'4294967295','unsigned OID index survives restart');
$node->stop;
done_testing();
