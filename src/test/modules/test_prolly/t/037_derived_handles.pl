# Row handles are derived from row content: the same row has the same TID on
# every branch and in every revision, and only duplicate rows, prefix
# collisions and recently used handles take allocated ones from the catalog.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('derived_handles');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
# The handle a ctid encodes, as dg_id computes it.
my $handle = q{((ctid::text::point)[0]::bigint * 291 + (ctid::text::point)[1]::bigint - 1)};
my $derived = 1 << 40;

sql(q{CREATE TABLE t(id int PRIMARY KEY, k int NOT NULL, v text); CREATE INDEX t_k ON t(k)});
sql(q{INSERT INTO t SELECT g, g % 10, 'v' || g FROM generate_series(1, 500) g});
is(sql(qq{SELECT count(*) FROM t WHERE $handle >= $derived}), '0', 'distinct rows take derived handles');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '0', 'and write no handle rows');
is(sql(q{SELECT count(DISTINCT ctid) FROM t}), '500', 'every row has its own handle');
is(sql(q{SET enable_seqscan=off; SELECT v FROM t WHERE id = 42}), 'v42', 'the index finds a row by its derived handle');

# The same row has the same handle everywhere: on a branch, in a revision.
sql(q{SELECT dolt_commit('-Am', 'base')});
sql(q{SELECT dolt_branch('other')});
my $tids = sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t WHERE id <= 20});
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM "postgres/other".public.t WHERE id <= 20}), $tids, 'a branch shares its rows\' handles');
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t AS OF 'HEAD' WHERE id <= 20}), $tids, 'a revision shows the same handles');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '0', 'creating the branch wrote no handle rows');

# An update gives the new version a handle derived from its content; the old one is free again.
my $old = sql(q{SELECT ctid FROM t WHERE id = 7});
sql(q{UPDATE t SET v = 'seven' WHERE id = 7});
my $new = sql(q{SELECT ctid FROM t WHERE id = 7});
isnt($new, $old, 'an update changes the handle');
is(sql(qq{SELECT $handle < $derived FROM t WHERE id = 7}), 't', 'to a derived one');
is(sql(q{SELECT ctid FROM "postgres/other".public.t WHERE id = 7}), $old, 'the other branch keeps the old version and handle');

# Duplicate rows: the first occurrence is derived, the rest allocated, one row each.
sql(q{CREATE TABLE dup(x int, y text); INSERT INTO dup VALUES (1, 'a'), (1, 'a'), (1, 'a'), (2, 'b')});
is(sql(q{SELECT count(DISTINCT ctid) FROM dup}), '4', 'duplicates have distinct handles');
is(sql(qq{SELECT count(*) FROM dup WHERE $handle >= $derived}), '2', 'two of which are allocated');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '2', 'with a catalog row each');
is(sql(q{SELECT count(*) FROM (SELECT ctid FROM dup WHERE x = 1 ORDER BY ctid LIMIT 1 FOR UPDATE) s}), '1', 'a duplicate occurrence locks on its own');
sql(q{DELETE FROM dup WHERE ctid = (SELECT ctid FROM dup WHERE x = 1 ORDER BY ctid DESC LIMIT 1)});
is(sql(q{SELECT count(*) FROM dup WHERE x = 1}), '2', 'deleting one occurrence by ctid removes one');
sql(q{DELETE FROM dup WHERE x = 1});
is(sql(q{SELECT count(*) FROM dup}), '1', 'deleting the row removes every occurrence');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '0', 'and their catalog rows');
sql(q{INSERT INTO dup VALUES (1, 'a'), (1, 'a')});
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '2', 'reinserted duplicates take allocated handles: a lock record names their derived one');

# A branch inherits derived handles for free and allocates for the duplicates it meets.
sql(q{SELECT dolt_commit('-Am', 'dup')});
sql(q{SELECT dolt_branch('dups')});
is(sql(q{SELECT count(DISTINCT ctid) FROM "postgres/dups".public.dup}), '3', 'a branch tells duplicates apart');
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY ctid) FROM "postgres/dups".public.dup WHERE x = 2}),
	sql(q{SELECT string_agg(ctid::text, ',' ORDER BY ctid) FROM dup WHERE x = 2}), 'and shares the distinct row\'s handle');
is(sql(q{SELECT count(DISTINCT ctid) FROM dup AS OF 'HEAD'}), '3', 'as does a revision');

# A handle a lock record still names is not derived again for a re-inserted row.
my $before = sql(q{SELECT ctid FROM t WHERE id = 100});
sql(q{DELETE FROM t WHERE id = 100});
sql(q{INSERT INTO t VALUES (100, 0, 'v100')});
my $after = sql(q{SELECT ctid FROM t WHERE id = 100});
isnt($after, $before, 'a re-inserted row whose handle has a lock record gets a new one');
is(sql(qq{SELECT $handle >= $derived FROM t WHERE id = 100}), 't', 'an allocated one');
sql(q{UPDATE t SET v = 'hundred' WHERE id = 100});
is(sql(q{SELECT v FROM t WHERE id = 100}), 'hundred', 'which locks and updates');
is(sql(q{SELECT count(*) FROM t WHERE id = 100}), '1', 'as one row');

# Lossy bitmap pages: a page is a digest range, scanned once.
sql(q{CREATE TABLE big(id int PRIMARY KEY, k int NOT NULL); CREATE INDEX big_k ON big(k)});
sql(q{INSERT INTO big SELECT g, g % 100 FROM generate_series(1, 20000) g});
my $expected = sql(q{SET enable_bitmapscan=off; SELECT count(*), sum(id) FROM big WHERE k BETWEEN 10 AND 60});
is(sql(q{SET enable_seqscan=off; SET enable_indexscan=off; SET work_mem='64kB'; SELECT count(*), sum(id) FROM big WHERE k BETWEEN 10 AND 60}),
	$expected, 'a lossy bitmap scan finds every row once');
is(sql(q{SET enable_seqscan=off; SET enable_indexscan=off; SET work_mem='64kB'; SELECT count(*) FROM big WHERE k BETWEEN 10 AND 60 AND id % 7 = 0}),
	sql(q{SET enable_bitmapscan=off; SELECT count(*) FROM big WHERE k BETWEEN 10 AND 60 AND id % 7 = 0}), 'and rechecks its qualification');

# Allocated handles continue past the committed ones after a crash.
sql(q{INSERT INTO dup VALUES (1, 'a')});
my $max = sql(qq{SELECT max($handle) FROM dup});
$node->stop('immediate');
$node->start;
sql(q{INSERT INTO dup VALUES (1, 'a')});
cmp_ok(sql(qq{SELECT max($handle) FROM dup}), '>', $max, 'allocation continues above committed handles after a crash');
is(sql(q{SELECT count(DISTINCT ctid), count(*) FROM dup WHERE x = 1}), '4|4', 'every duplicate keeps its own handle');
$node->stop;
done_testing();
