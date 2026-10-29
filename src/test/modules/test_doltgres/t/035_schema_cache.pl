# Decoded schemas are cached by address, in each session and shared between
# sessions: a new session builds its catalog without decoding schemas another
# session decoded, and the catalog it builds is the same. Reads of
# pg_dolt_object's hash index count the objects a transaction reads.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('schema_cache');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres',shift) }
sub fails {
 my ($query,$error,$name)=@_;
 my ($rc,$out,$err)=$node->psql('postgres',$query);
 like($err,$error,$name);
}

sql(q{CREATE TYPE pair AS (a int, b int);
CREATE TABLE parent(id int PRIMARY KEY, name text UNIQUE);
CREATE TABLE child(id int PRIMARY KEY, a int DEFAULT 5, b text DEFAULT 'x' CHECK (length(b) < 5),
  c numeric(5,2) NOT NULL DEFAULT 1.5, d int[] DEFAULT '{1,2}', e int GENERATED ALWAYS AS (a * 2) STORED,
  f int GENERATED ALWAYS AS IDENTITY, p pair, parent int REFERENCES parent(id));
CREATE INDEX child_a ON child(a) INCLUDE (b);
CREATE UNIQUE INDEX child_bc ON child(b DESC, c);
CREATE TABLE plain(id int PRIMARY KEY, v text);
INSERT INTO parent VALUES (1, 'one');
INSERT INTO child(id, b, parent) VALUES (1, 'one', 1);
SELECT dolt_commit('-Am', 'base')});

# Objects a new session reads to build its catalog and run |query|.
sub object_reads {
 my ($query)=@_;
 my @lines=split /\n/, sql("BEGIN; $query; SELECT pg_stat_get_xact_numscans(9103); COMMIT");
 return $lines[-1];
}

# The catalog rows built for the tables, as text.
my $describe=q{SELECT string_agg(x, E'\n' ORDER BY x) FROM (
 SELECT format('%s.%s %s %s %s %s %s', c.relname, a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull,
   a.attgenerated, a.attidentity, pg_get_expr(d.adbin, d.adrelid)) x
  FROM pg_class c JOIN pg_attribute a ON a.attrelid=c.oid AND a.attnum > 0
  LEFT JOIN pg_attrdef d ON d.adrelid=c.oid AND d.adnum=a.attnum
  WHERE c.relname IN ('parent','child','plain')
 UNION ALL SELECT format('%s %s', conname, pg_get_constraintdef(oid)) FROM pg_constraint WHERE conrelid='child'::regclass
 UNION ALL SELECT pg_get_indexdef(indexrelid) || ' ' || indcollation::text FROM pg_index WHERE indrelid IN ('child'::regclass, 'parent'::regclass)) s};

# Without sharing, a new session decodes every schema; with it, none that the
# session which created them decoded.
$node->append_conf('postgresql.conf',"dolt_shared_schema_cache_size=0\n");
$node->reload;
my $fresh=sql($describe);
my $cold=object_reads('SELECT count(*) FROM child, parent, plain');
is(object_reads('SELECT count(*) FROM child, parent, plain'), $cold, 'without sharing, each new session decodes every schema');
$node->append_conf('postgresql.conf',"dolt_shared_schema_cache_size=16MB\n");
$node->reload;
my $warm=object_reads('SELECT count(*) FROM child, parent, plain');
cmp_ok($cold - $warm, '>=', 8, "with sharing, a new session decodes no table or index schema ($cold reads, then $warm)");
my $cached=sql($describe);
is($cached, $fresh, 'a catalog built from shared schemas matches one decoded afresh');
is(sql($describe), $cached, 'a catalog built from shared schemas again matches');
is(sql(q{INSERT INTO child(id, parent) VALUES (2, 1); SELECT a, b, c, d, e, f FROM child WHERE id=2}), '5|x|1.50|{1,2}|10|2',
  'defaults, generated and identity columns apply from cached schemas');
fails(q{INSERT INTO child(id, b) VALUES (3, 'toolong')}, qr/violates check constraint "child_b_check"/, 'a check constraint from a cached schema is enforced');
fails(q{INSERT INTO child(id, b, parent) VALUES (4, 'y', 9)}, qr/violates foreign key constraint/, 'a foreign key from a cached schema is enforced');
fails(q{INSERT INTO child(id, b) VALUES (5, 'x')}, qr/duplicate key value violates unique constraint "child_bc"/, 'a unique index from a cached schema is enforced');

# Branches with different schemas for one table each see their own.
sql(q{SELECT dolt_branch('other')});
$node->safe_psql('postgres', q{SELECT dolt_checkout('other'); ALTER TABLE child ALTER COLUMN a SET DEFAULT 7; SELECT dolt_commit('-Am', 'seven')});
is($node->safe_psql('postgres', q{SELECT dolt_checkout('other'); INSERT INTO child(id, b) VALUES (20, 'o'); SELECT dolt_commit('-Am', 'row'); SELECT a FROM child WHERE id=20}) =~ s/.*\n//sr, '7',
  'a branch sees its own default');
is(sql(q{INSERT INTO child(id, b) VALUES (21, 'm'); SELECT a FROM child WHERE id=21}), '5', 'the other branch keeps its default');
is(sql(q{SELECT a FROM child AS OF 'other' WHERE id=20}), '7', 'a revision of the table reads its own schema');

# Evicted entries are decoded again, in a session and in shared memory.
is($node->safe_psql('postgres', q{SET dolt_schema_cache_size=0; SELECT count(*) FROM child; SELECT count(*) FROM child; SELECT a FROM child WHERE id=21}),
  "3\n3\n5", 'a session that evicts every schema when a transaction ends reads correctly');
$node->append_conf('postgresql.conf',"dolt_shared_schema_cache_size=1\n");
$node->reload;
for my $i (1..3) {
 is(sql($describe), $cached, "a catalog built while shared schemas are evicted matches ($i)");
}
$node->stop;
done_testing();
