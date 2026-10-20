use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('types');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\n");
$node->start;
$node->safe_psql('postgres', q{
CREATE TYPE mood AS ENUM ('happy','sad');
CREATE DOMAIN positive AS numeric(20,4) CHECK (VALUE > 0);
CREATE TYPE payload AS (n numeric, a integer[], t text, d positive);
});

# Ordinary built-ins, including less-common OID aliases and every built-in range.
# Expressions are evaluated first in heap, then copied to avoid testing merely
# whether the prolly encoder and decoder agree with each other.
my @cases = (
 ['pg_type', q{(SELECT t FROM pg_type t WHERE oid='int4'::regtype)}, q{(SELECT t FROM pg_type t WHERE oid='text'::regtype)}],
 ['pg_proc', q{(SELECT p FROM pg_proc p WHERE oid='int4in'::regproc)}, q{(SELECT p FROM pg_proc p WHERE oid='textin'::regproc)}],
 ['pg_class', q{(SELECT c FROM pg_class c WHERE oid='pg_type'::regclass)}, q{(SELECT c FROM pg_class c WHERE oid='pg_proc'::regclass)}],
 ['bool', 'true', 'false'],
 ['bytea', q{'\x00ff'}, q{''}],
 ['"char"', q{'a'}, q{''}],
 ['name', q{'éclair'}, q{''}],
 ['int2', '-32768', '32767'],
 ['int4', '-2147483648', '2147483647'],
 ['int8', '-9223372036854775808', '9223372036854775807'],
 ['int2vector', q{'1 -2 3'}, q{''}],
 ['oidvector', q{'0 1 4294967295'}, q{''}],
 ['oid', '0', '4294967295'],
 ['tid', q{'(12345,17)'}, q{'(0,1)'}],
 ['xid', q{'4294967295'}, q{'1'}],
 ['xid8', q{'18446744073709551615'}, q{'1'}],
 ['cid', q{'4294967295'}, q{'0'}],
 ['text', q{'éclair'}, q{''}],
 ['varchar(40)', q{'éclair'}, q{''}],
 ['char(8)', q{'ab  '}, q{''}],
 ['float4', q{'-Infinity'}, q{'NaN'}],
 ['float8', q{'-0'}, q{'Infinity'}],
 ['numeric', q{'12345678901234567890.1200'}, q{'NaN'}],
 ['numeric(20,4)', '123.4567', '-0.0001'],
 ['money', q{'1234.56'}, q{'-0.01'}],
 ['date', q{'infinity'}, q{'0001-01-01 BC'}],
 ['time(6)', q{'23:59:59.123456'}, q{'00:00:00'}],
 ['timetz', q{'23:59:59.123456-07:30'}, q{'00:00:00+00'}],
 ['timestamp', q{'2024-02-29 12:34:56.123456'}, q{'-infinity'}],
 ['timestamptz', q{'2024-02-29 12:34:56.123456+07:30'}, q{'infinity'}],
 ['interval', q{'1 year 2 mons -3 days 04:05:06.123456'}, q{'-1 microsecond'}],
 ['bit(7)', q{B'1010011'}, q{B'0000000'}],
 ['varbit', q{B'101001100'}, q{B''}],
 ['uuid', q{'550e8400-e29b-41d4-a716-446655440000'}, q{'00000000-0000-0000-0000-000000000000'}],
 ['inet', q{'2001:db8::1/64'}, q{'192.168.1.2/24'}],
 ['cidr', q{'2001:db8::/32'}, q{'192.168.0.0/16'}],
 ['macaddr', q{'08:00:2b:01:02:03'}, q{'ff:ff:ff:ff:ff:ff'}],
 ['macaddr8', q{'08:00:2b:01:02:03:04:05'}, q{'00:00:00:00:00:00:00:00'}],
 ['point', q{'(1.25,-2.5)'}, q{'(0,0)'}],
 ['line', q{'{1,2,3}'}, q{'{2,-1,0}'}],
 ['lseg', q{'[(1,2),(3,4)]'}, q{'[(0,0),(0,0)]'}],
 ['box', q{'(3,4),(1,2)'}, q{'(0,0),(0,0)'}],
 ['path', q{'[(1,2),(3,4)]'}, q{'((0,0),(1,1),(2,0))'}],
 ['polygon', q{'((0,0),(2,0),(1,1))'}, q{'((0,0))'}],
 ['circle', q{'<(1,2),3>'}, q{'<(0,0),0>'}],
 ['json', q{' { "é": 1.00, "a": [true,null] } '}, q{'null'}],
 ['jsonb', q{'{"é":1.00,"a":[true,null]}'}, q{'null'}],
 ['jsonpath', q{'$.a[*] ? (@ > 1)'}, q{'$."é"'}],
 ['tsvector', q{to_tsvector('english','a simple searchable document')}, q{''}],
 ['tsquery', q{to_tsquery('english','simple & document')}, q{''}],
 ['pg_lsn', q{'FFFFFFFF/FFFFFFFF'}, q{'0/0'}],
 ['txid_snapshot', q{'10:20:12,15'}, q{'1:1:'}],
 ['pg_snapshot', q{'10:20:12,15'}, q{'1:1:'}],
 ['refcursor', q{'cursor é'}, q{''}],
 ['regproc', q{'int4in'}, q{'0'}],
 ['regprocedure', q{'abs(integer)'}, q{'0'}],
 ['regoper', q{'0'}, q{'1'}],
 ['regoperator', q{'=(integer,integer)'}, q{'0'}],
 ['regclass', q{'pg_class'}, q{'0'}],
 ['regcollation', q{'"C"'}, q{'0'}],
 ['regtype', q{'numeric'}, q{'0'}],
 ['regrole', 'current_user', q{'0'}],
 ['regnamespace', q{'pg_catalog'}, q{'0'}],
 ['regconfig', q{'english'}, q{'simple'}],
 ['regdictionary', q{'english_stem'}, q{'simple'}],
 ['aclitem', q{(current_user||'=arwdDxt/'||current_user)::aclitem}, q{('=r/'||current_user)::aclitem}],
 ['int4range', q{'[1,10)'}, q{'empty'}],
 ['int8range', q{'[-9223372036854775808,9223372036854775807)'}, q{'(,)'}],
 ['numrange', q{'[1.00,2.50]'}, q{'empty'}],
 ['tsrange', q{'[2020-01-01,2021-01-01)'}, q{'(,)'}],
 ['tstzrange', q{'[2020-01-01+00,2021-01-01+00)'}, q{'empty'}],
 ['daterange', q{'[2020-01-01,2021-01-01)'}, q{'(,)'}],
 ['int4multirange', q{'{[1,3),[8,10)}'}, q{'{}'}],
 ['int8multirange', q{'{[1,3),[8,10)}'}, q{'{}'}],
 ['nummultirange', q{'{[1.00,2.50],[8.1,10.2)}'}, q{'{}'}],
 ['tsmultirange', q{'{[2020-01-01,2021-01-01)}'}, q{'{}'}],
 ['tstzmultirange', q{'{[2020-01-01+00,2021-01-01+00)}'}, q{'{}'}],
 ['datemultirange', q{'{[2020-01-01,2021-01-01)}'}, q{'{}'}],
 ['integer[]', q{'[-2:-1][3:4]={{1,NULL},{2,3}}'}, q{'{}'}],
 ['numeric[]', q{ARRAY[1.00,2.50,NULL]}, q{'{}'}],
 ['aclitem[]', q{ARRAY[('=r/'||current_user)::aclitem,NULL]}, q{'{}'}],
 ['text[]', q{ARRAY['é',NULL,'',repeat('z',20000)]}, q{'{}'}],
 ['int4range[]', q{ARRAY['[1,3)'::int4range,NULL]}, q{'{}'}],
 ['mood', q{'happy'}, q{'sad'}],
 ['positive', '1.2345', '99.0000'],
 ['positive[]', q{ARRAY[1.2345::positive,NULL]}, q{'{}'}],
 ['payload', q{ROW(1.00,ARRAY[1,2],repeat('é',12000),1.5)::payload}, q{ROW(NULL,NULL,'',NULL)::payload}],
 ['payload[]', q{ARRAY[ROW(1.00,NULL,'é',1)::payload,NULL]}, q{'{}'}],
);
my $has_xml = $node->safe_psql('postgres', q{SELECT position('--with-libxml' in setting)>0 FROM pg_config WHERE name='CONFIGURE'}) eq 't';
push @cases, ['xml', q{XMLPARSE(CONTENT '<a>é</a><b/>')}, q{XMLPARSE(DOCUMENT '<root/>')}] if $has_xml;

# Exercise generated array types too, including enum/composite/domain elements.
# The dedicated multidimensional cases above verify non-default lower bounds.
my @scalars = grep { $_->[0] !~ /\[\]/ } @cases;
for my $case (@scalars)
{
 my ($type, $a, $b) = @$case;
 push @cases, ["$type\[\]", "ARRAY[($a)::$type, NULL, ($b)::$type]", "'{}'"];
}

# Audit the catalog so an added built-in cannot silently escape this suite.
my $ordinary = $node->safe_psql('postgres', q{
SELECT typname FROM pg_type WHERE typnamespace='pg_catalog'::regnamespace
 AND typtype IN ('b','r','m') AND typcategory <> 'A'
 AND typname NOT IN ('pg_node_tree','pg_ndistinct','pg_dependencies',
                    'pg_mcv_list','gtsvector','pg_brin_bloom_summary',
                    'pg_brin_minmax_multi_summary')
 ORDER BY typname;
});
my %covered = map { my $t = $_->[0]; $t =~ s/\(.*\)//; $t =~ s/"//g; ($t => 1) } @scalars;
$covered{bpchar} = 1; # char(n)
$covered{xml} = 1 unless $has_xml; # build-time PostgreSQL feature
is_deeply([grep { !$covered{$_} } split /\n/, $ordinary], [], 'every ordinary built-in scalar/range has a fixture');

# The catalog inventory above includes explicitly rejected identity-bearing
# fixtures. Exercise their rejection rather than silently dropping coverage.
my %nonportable = map { $_ => 1 } qw(pg_type pg_proc pg_class oid oidvector tid xid xid8 cid pg_lsn txid_snapshot pg_snapshot refcursor regproc regprocedure regoper regoperator regclass regcollation regtype regrole regnamespace regconfig regdictionary aclitem);
my @portable;
for my $case (@cases)
{
 my $type = $case->[0];
 (my $base = $type) =~ s/\[\]//g;
 if ($nonportable{$base})
 {
  my ($status, $out, $err) = $node->psql('postgres', "CREATE TABLE rejected(v $type) USING dolt_prolly_table");
  isnt($status, 0, "$type rejected at table creation");
  like($err, qr/prolly storage rejects type/, "$type portability diagnostic");
 }
 else { push @portable, $case; }
}
@cases = @portable;

my @checks;
my $n = 0;
for my $case (@cases)
{
 my ($type, $a, $b) = @$case;
 ++$n;
 $node->safe_psql('postgres', "CREATE TABLE h$n(id int, v $type); CREATE TABLE p$n(LIKE h$n) USING dolt_prolly_table; CREATE TABLE q$n(LIKE h$n) USING dolt_prolly_table; INSERT INTO h$n VALUES (1,($a)::$type),(2,($b)::$type),(3,NULL),(4,($a)::$type); INSERT INTO p$n SELECT * FROM h$n ORDER BY id DESC;");
 my $query = "SELECT id,v::text FROM %s ORDER BY id";
 my $expected = $node->safe_psql('postgres', sprintf($query, "h$n"));
 is($node->safe_psql('postgres', sprintf($query, "p$n")), $expected, "$type matches heap values");
 # Same final bytes through UPDATE/DELETE versus a fresh INSERT history.
 $node->safe_psql('postgres', "UPDATE p$n SET v=($b)::$type WHERE id=1; DELETE FROM p$n WHERE id=4; UPDATE p$n SET v=h$n.v FROM h$n WHERE p$n.id=h$n.id; INSERT INTO p$n SELECT * FROM h$n WHERE id=4; INSERT INTO q$n SELECT * FROM h$n ORDER BY id; CREATE INDEX pi$n ON p$n(id) INCLUDE(v);");
 is($node->safe_psql('postgres', "SELECT dolt_prolly_root('p$n')=dolt_prolly_root('q$n')"), 't', "$type history independence");
 is($node->safe_psql('postgres', 'SET enable_seqscan=off; '.sprintf($query,"p$n")), $expected, "$type index-only payload");
 push @checks, [sprintf($query,"p$n"), $expected, $type];
}

my ($node_status, $node_out, $node_err) = $node->psql('postgres', q{CREATE TABLE nodes(v pg_node_tree) USING dolt_prolly_table});
isnt($node_status, 0, 'pg_node_tree rejects embedded local object references');
like($node_err, qr/prolly storage rejects type pg_node_tree/, 'node tree portability diagnostic');

# Table codecs must not require the old C collation restriction.
$node->safe_psql('postgres', q{
CREATE TABLE default_collation(v text) USING dolt_prolly_table;
INSERT INTO default_collation VALUES ('éclair');
});
is($node->safe_psql('postgres', 'SELECT v FROM default_collation'), 'éclair', 'default collation table values');

# Collation is a column/index semantic, not a table-codec restriction.
$node->safe_psql('postgres', q{
CREATE TABLE collated(v text COLLATE "POSIX", n numeric) USING dolt_prolly_table;
INSERT INTO collated VALUES ('é',1.00),('é',1.0);
CREATE TABLE scale(v text COLLATE "POSIX", n numeric) USING dolt_prolly_table;
INSERT INTO scale VALUES ('é',1.0),('é',1.00);
});
is($node->safe_psql('postgres', q{SELECT dolt_prolly_root('collated')=dolt_prolly_root('scale')}), 't', 'numeric scales retain deterministic encodings');
# Client encoding and display GUCs must not leak into persistent binary payloads.
$node->safe_psql('postgres', q{
CREATE TABLE settings(a varchar, b jsonb, c name, d text[], e timestamptz, f interval) USING dolt_prolly_table;
CREATE TABLE settings2(LIKE settings) USING dolt_prolly_table;
INSERT INTO settings VALUES (chr(233),jsonb_build_object(chr(233),1),chr(233),ARRAY[chr(233)],'2024-01-01+00','1 year 2 days');
SET client_encoding=LATIN1;
SET timezone='Pacific/Honolulu'; SET datestyle='SQL, DMY'; SET intervalstyle='sql_standard';
INSERT INTO settings2 SELECT * FROM settings;
});
is($node->safe_psql('postgres', q{SELECT dolt_prolly_root('settings')=dolt_prolly_root('settings2')}), 't', 'client encoding and display settings do not change roots');
is($node->safe_psql('postgres', q{SET client_encoding=LATIN1; SELECT ascii(a),ascii(b->>chr(233)),ascii(c),ascii(d[1]) FROM settings}), '233|49|233|233', 'binary receive uses database encoding');
is($node->safe_psql('postgres', q{SET client_encoding=LATIN1; SELECT count(*) FROM settings; SHOW client_encoding}), "1\nLATIN1", 'codec restores client encoding');
if ($has_xml)
{
 my ($xml) = grep { $_->[2] eq 'xml' } @checks;
 is($node->safe_psql('postgres', 'SET xmloption=DOCUMENT; '.$xml->[0]), $xml->[1], 'stored XML fragments read under DOCUMENT mode');
}
# Check binary values that text/equality comparisons alone could miss.
$node->safe_psql('postgres', q{
CREATE TABLE edge_heap(id int, f float8, n numeric);
INSERT INTO edge_heap VALUES (1,'-0',1.00),(2,'0',1.0),
 (3,'NaN','Infinity'),(4,'-Infinity','-Infinity'),(5,'5e-324',0.00000000000000000001);
CREATE TABLE edge_prolly(LIKE edge_heap) USING dolt_prolly_table;
INSERT INTO edge_prolly SELECT * FROM edge_heap;
});
my $binary_query = q{SELECT id,encode(float8send(f),'hex'),encode(numeric_send(n),'hex') FROM %s ORDER BY id};
is($node->safe_psql('postgres', sprintf($binary_query, 'edge_prolly')),
 $node->safe_psql('postgres', sprintf($binary_query, 'edge_heap')), 'binary float and numeric representations survive storage');
$node->safe_psql('postgres', q{
CREATE TABLE one_scale(n numeric) USING dolt_prolly_table;
CREATE TABLE another_scale(n numeric) USING dolt_prolly_table;
INSERT INTO one_scale VALUES (1.0); INSERT INTO another_scale VALUES (1.00);
});
is($node->safe_psql('postgres', q{SELECT dolt_prolly_root('one_scale')<>dolt_prolly_root('another_scale')}), 't', 'SQL-equal numeric scales may have different content hashes');
like($node->safe_psql('postgres', 'SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT id,v FROM p1 ORDER BY id'), qr/Index Only Scan/, 'payload checks exercise index-only scans');

# Reject unavailable ordered encodings rather than applying byte ordering to a
# generic binary representation (e.g. numeric or a collation-aware string).
my ($status, $stdout, $stderr) = $node->psql('postgres', q{CREATE INDEX unsupported_numeric ON one_scale(n)});
isnt($status, 0, 'new storage support does not claim numeric ordered keys');
like($stderr, qr/no default operator class/, 'missing ordered opclass is explicit');
$node->safe_psql('postgres', q{CREATE TABLE domains(v positive, a positive[]) USING dolt_prolly_table; INSERT INTO domains VALUES (1,ARRAY[2::positive])});
($status, $stdout, $stderr) = $node->psql('postgres', q{INSERT INTO domains VALUES (-1,NULL)});
isnt($status, 0, 'domain assignment constraints remain enforced');
$node->safe_psql('postgres', q{ALTER DOMAIN positive ADD CONSTRAINT later CHECK (VALUE > 1000) NOT VALID});
is($node->safe_psql('postgres', q{SELECT v,a FROM domains}), '1.0000|{2.0000}', 'reads do not recheck new domain constraints');

$node->stop('immediate');
$node->start;
for my $check (@checks)
{
 is($node->safe_psql('postgres', $check->[0]), $check->[1], "$check->[2] survives WAL crash recovery");
}
$node->stop;
done_testing();
