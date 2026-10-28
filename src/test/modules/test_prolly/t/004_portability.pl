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

# Explicit identifiers are covered by 003_types and 039_explicit_identifiers.
# Arbitrary serializers still cannot introduce incidental identifiers.
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

# UUIDs and ordinary numeric/text values are data, not typed local references.
$node->safe_psql('postgres', q{CREATE TABLE plain(n bigint, s text, u uuid) USING dolt_prolly_table; INSERT INTO plain VALUES (16432,'16432','550e8400-e29b-41d4-a716-446655440000')});
is($node->safe_psql('postgres', q{SELECT n,s FROM plain}), '16432|16432', 'ordinary values are not guessed to be OIDs');
$node->stop;
done_testing();
