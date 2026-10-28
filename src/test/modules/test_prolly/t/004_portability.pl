use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('portability');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\n");
$node->start;


sub rejected
{
 my ($sql, $label) = @_;
 my ($status, $out, $err) = $node->psql('postgres', $sql);
 isnt($status, 0, $label);
 like($err, qr/prolly storage rejects type/, "$label: codec policy error");
}

# Untranslated symbolic references and internal identifiers remain restricted.
# Explicit numeric OID payloads are allowed, including container wrappers.
my @identifiers = qw(regproc regprocedure regoper regoperator
 regclass regcollation regtype regrole regnamespace regconfig regdictionary
 aclitem pg_node_tree xid xid8 cid tid pg_lsn txid_snapshot pg_snapshot refcursor);
my $i = 0;
for my $type (@identifiers)
{
 ++$i;
 $node->safe_psql('postgres', "CREATE DOMAIN identifier$i AS $type; CREATE DOMAIN wrapped$i AS identifier$i; CREATE TYPE nested$i AS (safe int, unsafe wrapped$i)");
 for my $wrapper ("identifier$i", "wrapped$i", "nested$i", "nested$i\[\]")
 {
  rejected("CREATE TABLE forbidden(v $wrapper) USING dolt_prolly_table", "$wrapper containing $type rejected");
 }
}
# TODO: CREATE TABLE AS of a versioned table. It is rejected outright today;
# once supported, these must still be rejected by the type codec.
rejected(q{CREATE TABLE forbidden(v regclass) USING dolt_prolly_table}, 'NULL is not a schema bypass');
rejected(q{CREATE TABLE forbidden(v regclass[]) USING dolt_prolly_table}, 'empty array is not a schema bypass');
rejected(q{CREATE TABLE forbidden(v regclass) USING dolt_prolly_table}, 'empty table still validates schema');

# Uninspected binary serializers are not trusted merely because I/O exists.
$node->safe_psql('postgres', q{
CREATE TYPE opaque;
CREATE FUNCTION opaque_in(cstring) RETURNS opaque LANGUAGE internal IMMUTABLE STRICT AS 'oidin';
CREATE FUNCTION opaque_out(opaque) RETURNS cstring LANGUAGE internal IMMUTABLE STRICT AS 'oidout';
CREATE FUNCTION opaque_recv(internal) RETURNS opaque LANGUAGE internal IMMUTABLE STRICT AS 'oidrecv';
CREATE FUNCTION opaque_send(opaque) RETURNS bytea LANGUAGE internal IMMUTABLE STRICT AS 'oidsend';
CREATE TYPE opaque (INPUT=opaque_in, OUTPUT=opaque_out, RECEIVE=opaque_recv, SEND=opaque_send, INTERNALLENGTH=4, PASSEDBYVALUE, ALIGNMENT=int4);
CREATE TYPE local_range AS RANGE (SUBTYPE=oid);
CREATE TYPE array_range AS RANGE (SUBTYPE=integer[]);
});
rejected(q{CREATE TABLE forbidden(v opaque) USING dolt_prolly_table}, 'opaque custom OID sender rejected');
rejected(q{CREATE TABLE forbidden(v opaque[]) USING dolt_prolly_table}, 'nested opaque custom OID sender rejected');
# Explicit OID range payloads are tested in 038_index_types.pl.
rejected(q{CREATE TABLE forbidden(v array_range) USING dolt_prolly_table}, 'range delegating to array_send with embedded type OIDs rejected');

# Revalidate after nested schema changes, including writes of NULL and index
# rebuilds that copy stored bytes without re-encoding individual values.
# TODO: user-defined types in versioned tables.
SKIP: {
 skip 'user-defined types are not versioned yet', 8;
 $node->safe_psql('postgres', q{
 CREATE TYPE evolving AS (a int);
 CREATE TABLE existing(k int, v evolving) USING dolt_prolly_table;
 INSERT INTO existing VALUES (1,NULL);
 CREATE INDEX existing_idx ON existing(k) INCLUDE(v);
 ALTER TYPE evolving ADD ATTRIBUTE object regclass CASCADE;
 });
 rejected(q{INSERT INTO existing VALUES (2,NULL)}, 'changed nested schema blocks even NULL writes');
 rejected(q{SELECT * FROM existing}, 'changed nested schema blocks sequential reads');
 rejected(q{SET enable_seqscan=off; SELECT k,v FROM existing ORDER BY k}, 'changed nested schema blocks index-only decoding');
 rejected(q{CREATE INDEX another_idx ON existing(k)}, 'changed nested schema blocks index rebuild copying');
}

# Enum labels and recursive container codecs must not persist their type OIDs.
# Create independent types with identical value definitions and different OIDs.
# TODO: user-defined types in versioned tables.
SKIP: {
 skip 'user-defined types are not versioned yet', 3;
 $node->safe_psql('postgres', q{
 CREATE TYPE enum_a AS ENUM ('one','two');
 CREATE TYPE enum_b AS ENUM ('one','two');
 CREATE TYPE record_a AS (e enum_a[], n numeric, r int4range);
 CREATE TYPE record_b AS (e enum_b[], n numeric, r int4range);
 CREATE TABLE portable_a(v record_a) USING dolt_prolly_table;
 CREATE TABLE portable_b(v record_b) USING dolt_prolly_table;
 INSERT INTO portable_a VALUES (ROW(ARRAY['one'::enum_a,NULL,'two'::enum_a],1.00,'[1,3)'::int4range));
 INSERT INTO portable_b VALUES (ROW(ARRAY['one'::enum_b,NULL,'two'::enum_b],1.00,'[1,3)'::int4range));
 });
 is($node->safe_psql('postgres', q{SELECT 'enum_a'::regtype::oid <> 'enum_b'::regtype::oid AND 'record_a'::regtype::oid <> 'record_b'::regtype::oid}), 't', 'test types really have distinct runtime OIDs');
 is($node->safe_psql('postgres', q{SELECT dolt_prolly_root('portable_a')=dolt_prolly_root('portable_b')}), 't', 'enum/array/composite type OIDs do not enter serialized values');
 is($node->safe_psql('postgres', q{SELECT (v).e::text,(v).n,(v).r FROM portable_a}), '{one,NULL,two}|1.00|[1,3)', 'portable containers retain values');
}
# UUIDs and ordinary numeric/text values are data, not typed local references.
$node->safe_psql('postgres', q{CREATE TABLE plain(n bigint, s text, u uuid) USING dolt_prolly_table; INSERT INTO plain VALUES (16432,'16432','550e8400-e29b-41d4-a716-446655440000')});
is($node->safe_psql('postgres', q{SELECT n,s FROM plain}), '16432|16432', 'ordinary values are not guessed to be OIDs');
$node->stop;
done_testing();
