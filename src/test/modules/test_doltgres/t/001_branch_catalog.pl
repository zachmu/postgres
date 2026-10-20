# Copyright (c) 2026, PostgreSQL Global Development Group
# Branch catalog projection under real backend caches, snapshots and locks.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_catalog');
$node->init;
$node->append_conf('postgresql.conf', q{
session_preload_libraries = 'test_doltgres'
max_prepared_transactions = 10
});
$node->start;
$node->safe_psql('postgres', q{
CREATE SCHEMA dg_4_main_public;
CREATE SCHEMA dg_3_dev_public;
CREATE TABLE dg_4_main_public.t(id int PRIMARY KEY, value int);
CREATE TABLE dg_3_dev_public.t(id int PRIMARY KEY, value text, extra bool);
INSERT INTO "postgres/main".public.t VALUES(1,10);
INSERT INTO "postgres/dev".public.t VALUES(1,'ten',true);
});

is($node->safe_psql('postgres', q{
SELECT m.value, d.value, d.extra, pg_typeof(m.value), pg_typeof(d.value)
FROM "postgres/main".public.t m JOIN "postgres@dev".public.t d USING(id)
}), '10|ten|t|integer|text', 'different tuple descriptors coexist in one query');

my $reader = $node->background_psql('postgres');
$reader->query_safe(q{
PREPARE main_plan AS SELECT value FROM "postgres/main".public.t WHERE id=1;
PREPARE dev_plan AS SELECT value::text FROM "postgres/dev".public.t WHERE id=1;
});
is($reader->query_safe('EXECUTE main_plan'), '10', 'main prepared plan');
is($reader->query_safe('EXECUTE dev_plan'), 'ten', 'dev prepared plan');

$node->safe_psql('postgres', q{
ALTER TABLE "postgres/dev".public.t ALTER COLUMN value TYPE int USING length(value);
});
is($reader->query_safe('EXECUTE main_plan'), '10', 'other branch plan remains valid');
is($reader->query_safe('EXECUTE dev_plan'), '3', 'cross-backend DDL replans projected relation');

my $identity = $node->safe_psql('postgres', q{
SELECT 'dg_4_main_public.t'::regclass::oid
});
$node->safe_psql('postgres', q{
BEGIN;
ALTER TABLE "postgres/main".public.t ADD COLUMN transient text;
ROLLBACK;
});
is($node->safe_psql('postgres', q{
SELECT count(*) FROM pg_attribute
WHERE attrelid='dg_4_main_public.t'::regclass AND attname='transient' AND NOT attisdropped
}), '0', 'DDL rollback restores schema');
is($node->safe_psql('postgres', q{SELECT 'dg_4_main_public.t'::regclass::oid}),
   $identity, 'runtime relation identity survives transactions and DDL rollback');
is($reader->query_safe('EXECUTE main_plan'), '10', 'prepared plan survives DDL rollback');

# A lock on the main branch must not unnecessarily serialize a different branch.
my $locker = $node->background_psql('postgres');
$locker->query_safe(q{BEGIN; SELECT * FROM "postgres/main".public.t FOR UPDATE;});
is($node->safe_psql('postgres', q{
SET lock_timeout='200ms';
UPDATE "postgres/dev".public.t SET value=4 WHERE id=1 RETURNING value;
}), '4', 'different branch row lock identity');
my ($out, $err);
my $status = $node->psql('postgres', q{
SET lock_timeout='200ms';
UPDATE "postgres/main".public.t SET value=99 WHERE id=1;
}, stdout => \$out, stderr => \$err);
ok($status != 0, 'same branch writers still conflict');
like($err, qr/lock timeout/, 'normal PostgreSQL lock timeout');
$locker->query_safe('ROLLBACK');
$locker->quit;

# Ordinary snapshot isolation applies to both projected relations together.
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($reader->query_safe(q{
SELECT m.value,d.value FROM "postgres/main".public.t m, "postgres/dev".public.t d
}), '10|4', 'snapshot pins both branch views');
$node->safe_psql('postgres', q{
BEGIN;
UPDATE "postgres/main".public.t SET value=11;
UPDATE "postgres/dev".public.t SET value=5;
COMMIT;
});
is($reader->query_safe(q{
SELECT m.value,d.value FROM "postgres/main".public.t m, "postgres/dev".public.t d
}), '10|4', 'old snapshot retains consistent old branch values');
$reader->query_safe('COMMIT');
is($reader->query_safe(q{
SELECT m.value,d.value FROM "postgres/main".public.t m, "postgres/dev".public.t d
}), '11|5', 'new snapshot sees both committed branch changes');

$node->safe_psql('postgres', q{
BEGIN;
UPDATE "postgres/main".public.t SET value=12;
SAVEPOINT s;
ALTER TABLE "postgres/dev".public.t ADD COLUMN subtransaction_column int;
UPDATE "postgres/dev".public.t SET value=6;
ROLLBACK TO s;
COMMIT;
});
is($reader->query_safe(q{
SELECT m.value,d.value FROM "postgres/main".public.t m, "postgres/dev".public.t d
}), '12|5', 'subtransaction rollback restores only its branch changes');
is($node->safe_psql('postgres', q{
SELECT count(*) FROM pg_attribute WHERE attrelid='dg_3_dev_public.t'::regclass
AND attname='subtransaction_column' AND NOT attisdropped
}), '0', 'subtransaction DDL rolled back');
$reader->quit;

# This uses existing heap WAL, proving the projection does not bypass 2PC.
# It does not claim custom prolly durability has been implemented.
$node->safe_psql('postgres', q{
BEGIN;
UPDATE "postgres/main".public.t SET value=20;
UPDATE "postgres/dev".public.t SET value=30;
PREPARE TRANSACTION 'dg_branch_prepare';
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{
SELECT m.value,d.value FROM "postgres/main".public.t m, "postgres/dev".public.t d
}), '12|5', 'prepared changes remain invisible after crash recovery');
$node->safe_psql('postgres', q{COMMIT PREPARED 'dg_branch_prepare'});
is($node->safe_psql('postgres', q{
SELECT m.value,d.value FROM "postgres/main".public.t m, "postgres/dev".public.t d
}), '20|30', 'prepared decision commits both projected branches');

$node->stop;
done_testing();
