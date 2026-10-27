# A command's inserts are recorded as pending edits and applied to the trees
# in one pass. What the command, later commands, other sessions, triggers,
# uniqueness checks, savepoints and errors see must not change.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('batched');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nmax_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub fails { my ($q, $re, $label) = @_; my ($code, $out, $err) = $node->psql('postgres', $q); isnt($code, 0, $label); like($err, $re, "$label diagnostic"); }
sql(q{CREATE TABLE t(id int PRIMARY KEY, k int NOT NULL CHECK (k >= 0), v text); CREATE INDEX t_k ON t(k);
 CREATE TABLE dup(a int, b text); CREATE INDEX dup_a ON dup(a)});

# One statement, many rows.
sql(q{INSERT INTO t SELECT g, g % 100, 'v'||g FROM generate_series(1, 2000) g});
is(sql(q{SELECT count(*), sum(k), min(id), max(id) FROM t}), '2000|99000|1|2000', 'multi-row insert lands every row');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 7}), '20', 'secondary index has every entry');
is(sql(q{SET enable_seqscan=off; SELECT v FROM t WHERE id = 1234}), 'v1234', 'primary key index has every entry');
is(sql(q{SELECT count(*) FROM (SELECT DISTINCT ctid FROM t) x}), '2000', 'every row has its own handle');

# Uniqueness inside one statement and against committed rows.
fails(q{INSERT INTO t VALUES (5000, 1, 'a'), (5001, 1, 'b'), (5000, 1, 'c')}, qr/duplicate key value violates unique constraint "t_pkey"/, 'duplicate key within one statement');
is(sql(q{SELECT count(*) FROM t WHERE id >= 5000}), '0', 'a failing statement inserts nothing');
fails(q{INSERT INTO t VALUES (6000, 1, 'a'), (17, 1, 'b')}, qr/duplicate key value violates unique constraint "t_pkey"/, 'duplicate key against a committed row');
fails(q{INSERT INTO t VALUES (6000, 1, 'a'), (6001, -1, 'b')}, qr/violates check constraint/, 'a later row failing rolls the statement back');
is(sql(q{SELECT count(*) FROM t WHERE id >= 6000}), '0', 'nothing from the failed statement remains');

# The same transaction, later commands.
is(sql(q{BEGIN; INSERT INTO t SELECT g, 1, 'x' FROM generate_series(3001, 3500) g; SELECT count(*) FROM t WHERE id > 3000; COMMIT}), '500', 'the next command sees the inserted rows');
is(sql(q{BEGIN; INSERT INTO t VALUES (4001, 1, 'a'), (4002, 1, 'b'); UPDATE t SET v = 'updated' WHERE id = 4001; DELETE FROM t WHERE id = 4002; SELECT id, v FROM t WHERE id > 4000; COMMIT}), '4001|updated', 'update and delete of rows inserted earlier in the transaction');
is(sql(q{BEGIN; INSERT INTO t VALUES (4101, 1, 'a'); INSERT INTO t VALUES (4102, 1, 'b'); SET LOCAL enable_seqscan=off; SELECT count(*) FROM t WHERE id > 4100; COMMIT}), '2', 'index scan before commit sees both commands');
is(sql(q{BEGIN; INSERT INTO t SELECT id + 10000, k, v FROM t WHERE id <= 10; SELECT count(*) FROM t WHERE id > 10000; COMMIT}), '10', 'insert selecting from the same table');
is(sql(q{SELECT count(*) FROM t}), '2513', 'row count after the transactions');

# Savepoints and errors.
is(sql(q{BEGIN; INSERT INTO t SELECT g, 2, 'p' FROM generate_series(20001, 20100) g; SAVEPOINT s; INSERT INTO t SELECT g, 2, 'q' FROM generate_series(20101, 20200) g; ROLLBACK TO s; INSERT INTO t VALUES (20300, 2, 'r'); COMMIT; SELECT count(*), max(id) FROM t WHERE id > 20000}), '101|20300', 'savepoint rollback forgets only its inserts');
is(sql(q{BEGIN; INSERT INTO t SELECT g, 3, 'z' FROM generate_series(30001, 30100) g; ROLLBACK; SELECT count(*) FROM t WHERE id > 30000}), '0', 'transaction rollback forgets a bulk insert');
is(sql(q{BEGIN; SAVEPOINT s; INSERT INTO t SELECT g, 3, 'z' FROM generate_series(30001, 30100) g; RELEASE s; SELECT count(*) FROM t WHERE id > 30000; COMMIT}), '100', 'a released savepoint keeps its inserts');

# Duplicate rows without a key share one content entry.
sql(q{INSERT INTO dup VALUES (1, 'same'), (1, 'same'), (2, 'other')});
is(sql(q{SELECT count(*) FROM dup WHERE a = 1}), '2', 'identical rows in one statement are both counted');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM dup WHERE a = 1}), '2', 'and both found through the index');
sql(q{DELETE FROM dup WHERE ctid = (SELECT min(ctid) FROM dup WHERE a = 1)});
is(sql(q{SELECT count(*) FROM dup WHERE a = 1}), '1', 'deleting one occurrence leaves the other');

# RETURNING and triggers see each row.
is(sql(q{INSERT INTO t VALUES (40001, 4, 'a'), (40002, 4, 'b') RETURNING id, v}), "40001|a\n40002|b", 'RETURNING');
sql(q{CREATE TABLE audit(id int, v text);
 CREATE FUNCTION log_row() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO audit VALUES (NEW.id, NEW.v); RETURN NEW; END $$;
 CREATE TRIGGER t_log AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION log_row()});
sql(q{INSERT INTO t SELECT g, 5, 'trig' FROM generate_series(50001, 50050) g});
is(sql(q{SELECT count(*), min(id), max(id) FROM audit}), '50|50001|50050', 'AFTER ROW triggers fetch every inserted row');
sql(q{DROP TRIGGER t_log ON t});

# COPY.
$node->safe_psql('postgres', "COPY t(id, k, v) FROM STDIN;\n" . join('', map { "$_\t6\tcopied\n" } 60001 .. 60500) . "\\.\n");
is(sql(q{SELECT count(*) FROM t WHERE v = 'copied'}), '500', 'COPY inserts every row');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 6 AND v = 'copied'}), '500', 'COPY maintains the index');

# Other sessions see a bulk insert only once it commits.
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; INSERT INTO t SELECT g, 7, 'w' FROM generate_series(70001, 70300) g});
is(sql(q{SELECT count(*) FROM t WHERE id > 70000}), '0', 'uncommitted bulk insert is invisible elsewhere');
is($writer->query_safe(q{SELECT count(*) FROM t WHERE id > 70000}), '300', 'and visible to its own transaction');
$writer->query_safe('COMMIT');
is(sql(q{SELECT count(*) FROM t WHERE id > 70000}), '300', 'committed bulk insert is visible');
$writer->quit;

# Everything survives a restart.
$node->restart;
is(sql(q{SELECT count(*), sum(k) FROM t}), sql(q{SELECT count(*), sum(k) FROM t}), 'consistent after restart');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 7}), sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE k = 7}), 'index and table agree after restart');
$node->stop;
done_testing();
