# Updates and deletes record their table edits in the command's batch, like
# inserts, applied to the trees in one pass. What the command, later commands,
# other sessions, uniqueness checks, savepoints and restarts see must not change.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('changes');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nmax_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub fails { my ($q, $re, $label) = @_; my ($code, $out, $err) = $node->psql('postgres', $q); isnt($code, 0, $label); like($err, $re, "$label diagnostic"); }
sql(q{CREATE TABLE t(id int PRIMARY KEY, k int NOT NULL, v text); CREATE INDEX t_k ON t(k);
 INSERT INTO t SELECT g, g % 100, 'v'||g FROM generate_series(1, 2000) g;
 CREATE TABLE dup(a int, b text); CREATE INDEX dup_a ON dup(a); INSERT INTO dup VALUES (1, 'same'), (1, 'same'), (2, 'other')});

# One statement, many rows.
sql(q{UPDATE t SET k = k + 1, v = v || '!' WHERE id <= 1000});
is(sql(q{SELECT count(*), sum(k), count(*) FILTER (WHERE v LIKE '%!') FROM t}), '2000|100000|1000', 'multi-row update changes every row once');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 1}), '20', 'secondary index follows the update');
is(sql(q{SET enable_seqscan=off; SELECT v FROM t WHERE id = 1000}), 'v1000!', 'primary key index finds the new version');
is(sql(q{SELECT count(*) FROM (SELECT DISTINCT ctid FROM t) x}), '2000', 'every row has one handle');
sql(q{DELETE FROM t WHERE k % 2 = 0 AND id > 200});
is(sql(q{SELECT count(*), count(*) FILTER (WHERE k % 2 = 0 AND id > 200) FROM t}), '1100|0', 'multi-row delete removes every matching row');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 2}), '2', 'index entries of deleted rows are gone');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 1}), '20', 'and the others remain');

# Uniqueness against rows the same statement changes.
is(sql(q{WITH u AS (UPDATE t SET id = id + 10000 WHERE id > 1990 RETURNING id) SELECT string_agg(id::text, ',' ORDER BY id) FROM u}), "11991,11993,11995,11997,11999", 'key change in a multi-row update');
is(sql(q{SELECT count(*) FROM t WHERE id > 10000}), '5', 'the old keys are free');
sql(q{UPDATE t SET v = 'same' WHERE id = 5});
is(sql(q{SELECT v FROM t WHERE id = 5}), 'same', 'update without a key change keeps the key');
fails(q{UPDATE t SET id = 7 WHERE id = 5}, qr/duplicate key/, 'moving a key onto a live row');
is(sql(q{SELECT count(*) FROM t WHERE id IN (5, 7)}), '2', 'the failed update changes nothing');
is(sql(q{BEGIN; DELETE FROM t WHERE id = 7; UPDATE t SET id = 7 WHERE id = 5; SELECT id, v FROM t WHERE id = 7; COMMIT}), '7|same', 'a key freed earlier in the transaction can be reused');
is(sql(q{MERGE INTO t USING (VALUES (9), (10)) AS s(id) ON t.id = s.id WHEN MATCHED THEN DELETE WHEN NOT MATCHED THEN INSERT VALUES (s.id, 1, 'merged'); SELECT count(*) FROM t WHERE id IN (9, 10)}), '0', 'MERGE deletes matched rows');
is(sql(q{MERGE INTO t USING (VALUES (9), (10)) AS s(id) ON t.id = s.id WHEN MATCHED THEN DELETE WHEN NOT MATCHED THEN INSERT VALUES (s.id, 1, 'merged'); SELECT string_agg(v, ',') FROM t WHERE id IN (9, 10)}), 'merged,merged', 'and inserts the unmatched ones');

# The same transaction, later commands.
is(sql(q{BEGIN; UPDATE t SET v = 'first' WHERE id = 11; UPDATE t SET v = v || ',second' WHERE id = 11; SELECT v FROM t WHERE id = 11; COMMIT}), 'first,second', 'a later command updates the pending version');
is(sql(q{BEGIN; UPDATE t SET v = 'gone' WHERE id = 13; DELETE FROM t WHERE v = 'gone'; SELECT count(*) FROM t WHERE id = 13; COMMIT}), '0', 'a later command deletes the pending version');
is(sql(q{BEGIN; DELETE FROM t WHERE id = 15; INSERT INTO t VALUES (15, 1, 'back'); SELECT v FROM t WHERE id = 15; COMMIT}), 'back', 'a deleted key is reinserted by a later command');
is(sql(q{BEGIN; UPDATE t SET k = k + 1000 WHERE id BETWEEN 100 AND 110; SET LOCAL enable_seqscan=off; SELECT count(*) FROM t WHERE k > 1000; COMMIT}), '11', 'index scan before commit sees the pending update');
is(sql(q{WITH u AS (UPDATE t SET v = 'j' FROM t t2 WHERE t.id = t2.id AND t.id < 20 RETURNING t.id) SELECT string_agg(id::text, ',' ORDER BY id) FROM u}), sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t WHERE id < 20}), 'self-join update returns each row');

# Duplicate rows without a key share one content entry.
sql(q{UPDATE dup SET b = 'changed' WHERE ctid = (SELECT min(ctid) FROM dup WHERE a = 1)});
is(sql(q{SELECT string_agg(b, ',' ORDER BY b) FROM dup WHERE a = 1}), 'changed,same', 'updating one occurrence leaves the other');
sql(q{DELETE FROM dup WHERE b = 'same'});
is(sql(q{SELECT count(*) FROM dup}), '2', 'deleting the last occurrence');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM dup WHERE a = 1}), '1', 'index agrees');

# Savepoints and errors.
is(sql(q{BEGIN; UPDATE t SET v = 'kept' WHERE id = 21; SAVEPOINT s; DELETE FROM t WHERE id = 21; UPDATE t SET v = 'lost' WHERE id = 23; ROLLBACK TO s; COMMIT; SELECT string_agg(v, ',' ORDER BY id) FROM t WHERE id IN (21, 23)}), 'kept,v23!', 'savepoint rollback forgets only its changes');
is(sql(q{BEGIN; DELETE FROM t WHERE id < 50; ROLLBACK; SELECT count(*) FROM t WHERE id < 50}), '47', 'transaction rollback forgets a bulk delete');
fails(q{UPDATE t SET k = CASE WHEN id = 27 THEN NULL ELSE k END WHERE id < 50}, qr/null value/, 'a failing update');
is(sql(q{SELECT count(*) FROM t WHERE k IS NULL}), '0', 'leaves no row changed');

# Triggers and RETURNING see each row.
sql(q{CREATE TABLE audit(id int, v text);
 CREATE FUNCTION t_log() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO audit VALUES (OLD.id, COALESCE(NEW.v, 'deleted')); RETURN NULL; END $$;
 CREATE TRIGGER t_log AFTER UPDATE OR DELETE ON t FOR EACH ROW EXECUTE FUNCTION t_log()});
sql(q{UPDATE t SET v = 'audited' WHERE id BETWEEN 30 AND 40; DELETE FROM t WHERE id BETWEEN 41 AND 45});
is(sql(q{SELECT count(*) FILTER (WHERE v = 'audited'), count(*) FILTER (WHERE v = 'deleted') FROM audit}), '11|5', 'AFTER ROW triggers see every change');
sql(q{DROP TRIGGER t_log ON t});

# Other sessions see changes only once they commit, and wait for row locks.
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; UPDATE t SET k = k + 100 WHERE id = 1; DELETE FROM t WHERE id = 3});
is(sql(q{SELECT k FROM t WHERE id = 1}), '2', 'uncommitted update is invisible elsewhere');
is(sql(q{SELECT count(*) FROM t WHERE id = 3}), '1', 'uncommitted delete is invisible elsewhere');
is($writer->query_safe(q{SELECT k FROM t WHERE id = 1}), '102', 'and visible to its own transaction');
my $blocked = $node->background_psql('postgres');
$blocked->query_until(qr/started/, q{\echo started
UPDATE t SET k = k + 1 WHERE id = 1;
});
$writer->query_safe('COMMIT');
$blocked->quit;
is(sql(q{SELECT k FROM t WHERE id = 1}), '103', 'a blocked update applies on top of the committed version');
is(sql(q{SELECT count(*) FROM t WHERE id = 3}), '0', 'committed delete is visible');
$writer->quit;

# An emptied table and everything else survive a restart.
sql(q{CREATE TABLE emptied(id int); INSERT INTO emptied SELECT g FROM generate_series(1, 100) g; TRUNCATE emptied; DELETE FROM dup});
my $before = sql(q{SELECT count(*), sum(k), sum(id) FROM t});
$node->restart;
is(sql(q{SELECT count(*), sum(k), sum(id) FROM t}), $before, 'consistent after restart');
is(sql(q{SELECT count(*) FROM emptied}), '0', 'a truncated table reads as empty after restart');
is(sql(q{SELECT count(*) FROM dup}), '0', 'a table emptied by delete reads as empty after restart');
sql(q{INSERT INTO emptied VALUES (1); INSERT INTO dup VALUES (3, 'again')});
is(sql(q{SELECT (SELECT count(*) FROM emptied), (SELECT count(*) FROM dup)}), '1|1', 'and accept rows again');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 1}), sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE k = 1}), 'index and table agree after restart');
$node->stop;
done_testing();
