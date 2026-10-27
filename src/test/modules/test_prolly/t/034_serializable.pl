# Serializable isolation on prolly tables. Predicate locks cover whole tables;
# readers conflict out to running and since-committed writers of a table.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('serializable');
$node->init(extra => [ '--encoding=UTF8', '--locale=C' ]);
$node->append_conf('postgresql.conf', "default_table_access_method=dolt_prolly_table\n");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub session
{
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
	return $s;
}

sub fails_serialization
{
	my ($s, $query, $label) = @_;

	$s->query($query);
	like($s->{stderr}, qr/could not serialize access due to read\/write dependencies/, $label);
	$s->query('ROLLBACK');
	$s->{stderr} = '';
}

sql(q{CREATE TABLE doctors(name text PRIMARY KEY, on_call bool);
INSERT INTO doctors VALUES ('a', true), ('b', true);
CREATE TABLE d1(id int); CREATE TABLE d2(id int);
INSERT INTO d1 VALUES (1); INSERT INTO d2 VALUES (1);
CREATE TABLE keys(i int PRIMARY KEY)});

# Write skew: each reads what the other writes.
my $a = session();
my $b = session();
$a->query_safe(q{SELECT count(*) FROM doctors WHERE on_call});
$b->query_safe(q{SELECT count(*) FROM doctors WHERE on_call});
$a->query_safe(q{UPDATE doctors SET on_call = false WHERE name = 'a'});
$b->query_safe(q{UPDATE doctors SET on_call = false WHERE name = 'b'});
$a->query_safe('COMMIT');
fails_serialization($b, 'COMMIT', 'write skew fails the second committer');
is(sql(q{SELECT count(*) FROM doctors WHERE on_call}), '1', 'one doctor stays on call');

# A reader of a table conflicts out to a writer whose change is still private:
# s2 reads d1 behind s1's update and s3 reads d2 behind s2's, as heap's
# readers would find those versions in progress.
my $c = session();
$a->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
$b->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
$a->query_safe('UPDATE d1 SET id = id + 1');
$b->query_safe('UPDATE d2 SET id = (SELECT id + 1 FROM d1)');
is($c->query_safe('SELECT id FROM d2'), '1', 'the third reader sees neither change');
$a->query_safe('COMMIT');
$c->query_safe('COMMIT');
fails_serialization($b, 'COMMIT', 'the pivot of a read-only anomaly fails');

# Concurrent inserters of a key both read the empty table: the loser fails
# serialization rather than uniqueness, as with heap.
$a->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
$b->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
$a->query_safe('SELECT * FROM keys');
$b->query_safe('SELECT * FROM keys');
$a->query_safe('INSERT INTO keys VALUES (42)');
$b->query_until(qr/started/, "\\echo started\nINSERT INTO keys VALUES (42);\n");
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'}),
	'the second inserter waits for the first');
$a->query_safe('COMMIT');
fails_serialization($b, 'SELECT 1', 'the second inserter fails serialization');

# Transactions on unrelated tables do not conflict.
$a->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
$b->query_safe('BEGIN ISOLATION LEVEL SERIALIZABLE');
$a->query_safe('SELECT * FROM d1; INSERT INTO d1 VALUES (5)');
$b->query_safe('SELECT * FROM d2; INSERT INTO d2 VALUES (5)');
$a->query_safe('COMMIT');
$b->query_safe('COMMIT');
is(sql('SELECT (SELECT count(*) FROM d1) + (SELECT count(*) FROM d2)'), '4',
	'writers of disjoint tables both commit');

$a->quit;
$b->quit;
$c->quit;
$node->stop;
done_testing();
