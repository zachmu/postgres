# Copyright (c) 2026, PostgreSQL Global Development Group

# Concurrent inserts of the same chunk wait for the first inserter instead of
# failing, and committed chunks survive crash recovery while uncommitted ones
# do not.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunks');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_dolt_chunk');

sub rows
{
	my ($data) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = sha256('$data')");
}

# The second inserter waits until the first decides, for commit and for abort.
foreach my $outcome ('COMMIT', 'ROLLBACK')
{
	my $data = "shared-$outcome";
	my $first = $node->background_psql('postgres');
	my $second = $node->background_psql('postgres');

	$first->query_safe('BEGIN');
	is($first->query_safe("SELECT test_chunk_put(sha256('$data'), '$data')"),
		't', "first inserter inserts ($outcome)");
	$second->query_until(qr/started/,
		"\\echo started\nSELECT test_chunk_put(sha256('$data'), '$data');\n");
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE '%test_chunk_put%'"
		),
		"second inserter waits ($outcome)");
	$first->query_safe($outcome);
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE '%test_chunk_put%'"
		),
		"second inserter resumes ($outcome)");
	$second->quit;
	$first->quit;
	is(rows($data), '1', "exactly one row after $outcome");
}

# Overlapping concurrent loads insert each chunk once, with no errors.
my @sessions = map { $node->background_psql('postgres') } 1 .. 4;
for my $i (0 .. $#sessions)
{
	my $first = $i * 10000;
	$sessions[$i]->query_until(qr/started/,
		"\\echo started\nSELECT test_chunk_bench_put(7, $first, 20000, 512);\n\\echo finished\n");
}
$_->quit for @sessions;
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_dolt_chunk WHERE length(chunkdata) = 512"),
	'50000',
	'overlapping loads store each chunk once');
is( $node->safe_psql(
		'postgres',
		'SELECT count(*) = count(DISTINCT chunkhash) FROM pg_dolt_chunk'),
	't',
	'no duplicate hashes');

# Committed chunks survive a crash; an in-progress insert does not.
$node->safe_psql('postgres', "SELECT test_chunk_put(sha256('durable'), 'durable')");
my $pending = $node->background_psql('postgres');
$pending->query_safe('BEGIN');
$pending->query_safe("SELECT test_chunk_put(sha256('pending'), 'pending')");
$node->stop('immediate');
$pending->quit;
$node->start;
is( $node->safe_psql(
		'postgres', "SELECT test_chunk_get(sha256('durable')) = 'durable'"),
	't',
	'committed chunk recovered');
is( $node->safe_psql('postgres', "SELECT test_chunk_exists(sha256('pending'))"),
	'f', 'uncommitted chunk absent after recovery');
is($node->safe_psql('postgres', "SELECT test_chunk_put(sha256('pending'), 'pending')"),
	't', 'uncommitted chunk can be inserted again');

$node->stop;
done_testing();
