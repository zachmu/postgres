# Triggers on versioned tables are versioned schema members of their own,
# branched with the rest of the schema.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('versioned_triggers');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}

sql(q{
CREATE TABLE t(id int PRIMARY KEY, v text, n int DEFAULT 0);
CREATE TABLE log(id serial PRIMARY KEY, what text);
CREATE FUNCTION upcase() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.v := upper(NEW.v) || coalesce(TG_ARGV[0], ''); RETURN NEW; END$$;
CREATE FUNCTION note() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN INSERT INTO log(what) VALUES (TG_NAME || ':' || TG_OP || ':' || (SELECT count(*) FROM changed)); RETURN NULL; END$$;
CREATE FUNCTION bump() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.n := OLD.n + 1; RETURN NEW; END$$;
});

# ---- firing ----
sql(q{CREATE TRIGGER t_upcase BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION upcase('!')});
sql(q{INSERT INTO t(id, v) VALUES (1, 'a')});
is(sql(q{SELECT v FROM t}),'A!','row trigger with arguments fires');
sql(q{CREATE TRIGGER t_note AFTER UPDATE ON t REFERENCING NEW TABLE AS changed FOR EACH STATEMENT EXECUTE FUNCTION note()});
sql(q{CREATE TRIGGER t_bump BEFORE UPDATE OF v ON t FOR EACH ROW WHEN (OLD.v IS DISTINCT FROM NEW.v) EXECUTE FUNCTION bump()});
sql(q{UPDATE t SET v = 'b'; UPDATE t SET v = 'b'; UPDATE t SET n = n});
is(sql(q{SELECT n FROM t}),'1','UPDATE OF columns and WHEN condition limit a row trigger');
is(sql(q{SELECT string_agg(what, ',' ORDER BY id) FROM log}),'t_note:UPDATE:1,t_note:UPDATE:1,t_note:UPDATE:1','statement trigger reads its transition table');
is(sql(q{SELECT pg_get_triggerdef(oid) FROM pg_trigger WHERE tgname='t_bump'}),
   'CREATE TRIGGER t_bump BEFORE UPDATE OF v ON public.t FOR EACH ROW WHEN ((old.v IS DISTINCT FROM new.v)) EXECUTE FUNCTION bump()',
   'trigger definition deparses as native');
is(sql(q{BEGIN; CREATE TRIGGER t_locked BEFORE DELETE ON t FOR EACH ROW EXECUTE FUNCTION upcase(); SELECT mode FROM pg_locks WHERE locktype='relation' AND relation='t'::regclass AND pid=pg_backend_pid() AND mode='ShareRowExclusiveLock'; ROLLBACK}),'ShareRowExclusiveLock','CREATE TRIGGER locks its table as native');
fails(q{ALTER TABLE t DROP COLUMN v},qr/trigger t_bump on table (public\.)?t depends on column v/,'trigger columns cannot be dropped alone');
fails(q{DROP FUNCTION bump()},qr/trigger t_bump on table t depends on function bump/,'a trigger function cannot be dropped alone');

# ---- enabling, disabling and comments ----
sql(q{ALTER TABLE t DISABLE TRIGGER t_upcase; INSERT INTO t(id, v) VALUES (5, 'e')});
is(sql(q{SELECT v FROM t WHERE id=5}),'e','a disabled trigger does not fire');
sql(q{ALTER TABLE t ENABLE ALWAYS TRIGGER t_upcase; ALTER TABLE t DISABLE TRIGGER USER; ALTER TABLE t ENABLE TRIGGER t_note});
is(sql(q{SELECT string_agg(tgname || ':' || tgenabled::text, ',' ORDER BY tgname) FROM pg_trigger WHERE NOT tgisinternal}),'t_bump:D,t_note:O,t_upcase:D','ENABLE and DISABLE TRIGGER change a versioned trigger\'s state');
sql(q{ALTER TABLE t ENABLE TRIGGER USER; DELETE FROM t WHERE id=5; COMMENT ON TRIGGER t_bump ON t IS 'counts changes'});
is(sql(q{SELECT obj_description(oid, 'pg_trigger') FROM pg_trigger WHERE tgname='t_bump'}),'counts changes','COMMENT ON TRIGGER');

# ---- branches ----
sql(q{SELECT dolt_commit('-Am','triggers'); SELECT dolt_branch('other')});
sql(q{SELECT dolt_checkout('other'); DROP TRIGGER t_upcase ON t; CREATE OR REPLACE TRIGGER t_note AFTER INSERT ON t REFERENCING NEW TABLE AS changed FOR EACH STATEMENT EXECUTE FUNCTION note(); INSERT INTO t(id, v) VALUES (2, 'c')});
is(sql(q{SELECT v FROM "postgres/other".public.t WHERE id=2}),'c','a dropped trigger does not fire on its branch');
is(sql(q{SELECT what FROM "postgres/other".public.log ORDER BY id DESC LIMIT 1}),'t_note:INSERT:1','a replaced trigger fires on its branch');
sql(q{INSERT INTO t(id, v) VALUES (2, 'c')});
is(sql(q{SELECT v FROM t WHERE id=2}),'C!','main keeps its own triggers');
is(sql(q{SELECT string_agg(tgname, ',' ORDER BY tgname) FROM "postgres/other".pg_catalog.pg_trigger WHERE NOT tgisinternal}),'t_bump,t_note','another branch\'s working triggers are readable');
is(sql(q{SELECT string_agg(tgname, ',' ORDER BY tgname) FROM pg_trigger AS OF 'other' WHERE NOT tgisinternal}),'t_bump,t_note,t_upcase','another branch\'s committed triggers are readable');
is(sql(q{SELECT string_agg(tgname, ',' ORDER BY tgname) FROM pg_trigger AS OF 'HEAD' WHERE NOT tgisinternal}),'t_bump,t_note,t_upcase','committed triggers are readable');
sql(q{SELECT dolt_checkout('other'); ALTER TABLE t DISABLE TRIGGER t_bump; SELECT dolt_checkout('main')});
is(sql(q{SELECT tgenabled FROM pg_trigger WHERE tgname='t_bump'}).sql(q{SELECT tgenabled FROM "postgres/other".pg_catalog.pg_trigger WHERE tgname='t_bump'}),'OD','a trigger\'s state is branched');
is(sql(q{SELECT obj_description(oid, 'pg_trigger') FROM pg_trigger AS OF 'HEAD' WHERE tgname='t_bump'}),'counts changes','a trigger\'s comment is versioned');

# ---- constraint triggers ----
sql(q{CREATE TABLE c(id int PRIMARY KEY, n int); CREATE TABLE peer(id int);
CREATE FUNCTION positive() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN IF NEW.n < 0 THEN RAISE EXCEPTION 'negative %', NEW.n; END IF; RETURN NULL; END$$;
CREATE CONSTRAINT TRIGGER c_positive AFTER INSERT OR UPDATE ON c FROM peer DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION positive()});
is(sql(q{SELECT pg_get_triggerdef(oid) FROM pg_trigger WHERE tgname='c_positive'}),
   'CREATE CONSTRAINT TRIGGER c_positive AFTER INSERT OR UPDATE ON public.c FROM peer DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION positive()',
   'a constraint trigger deparses as native');
is(sql(q{SELECT contype::text || condeferrable::text || condeferred::text FROM pg_constraint WHERE conname='c_positive'}),'ttruetrue','a constraint trigger has its constraint');
is(sql(q{BEGIN; INSERT INTO c VALUES (1, -1); SELECT count(*) FROM c; ROLLBACK}),'1','a deferred constraint trigger waits for commit');
fails(q{BEGIN; INSERT INTO c VALUES (1, -1); COMMIT},qr/negative -1/,'a deferred constraint trigger fires at commit');
fails(q{BEGIN; INSERT INTO c VALUES (1, -1); SET CONSTRAINTS c_positive IMMEDIATE; COMMIT},qr/negative -1/,'SET CONSTRAINTS IMMEDIATE fires it at once');
fails(q{CREATE CONSTRAINT TRIGGER c_pkey AFTER DELETE ON c FOR EACH ROW EXECUTE FUNCTION positive()},qr/duplicate key value violates unique constraint/,'a constraint trigger cannot take a constraint\'s name');
sql(q{ALTER TABLE peer RENAME TO peer2; ALTER TRIGGER c_positive ON c RENAME TO c_checked});
is(sql(q{SELECT conname FROM pg_constraint WHERE contype='t'}).':'.sql(q{SELECT pg_get_triggerdef(oid) FROM pg_trigger WHERE tgname='c_checked'}),
   'c_checked:CREATE CONSTRAINT TRIGGER c_checked AFTER INSERT OR UPDATE ON public.c FROM peer2 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION positive()',
   'renames follow a constraint trigger and its constraint');
sql(q{DROP TABLE peer2});
is(sql(q{SELECT count(*) FROM pg_trigger WHERE tgname='c_checked'}).sql(q{SELECT count(*) FROM pg_constraint WHERE contype='t'}),'00','dropping its FROM table drops a constraint trigger, as native');

# ---- drops ----
sql(q{DROP FUNCTION bump() CASCADE});
is(sql(q{SELECT count(*) FROM pg_trigger WHERE tgname='t_bump'}),'0','DROP FUNCTION CASCADE removes its triggers');
sql(q{DROP TABLE t});
is(sql(q{SELECT count(*) FROM pg_trigger WHERE NOT tgisinternal}),'0','DROP TABLE removes its triggers');
is(sql(q{SELECT count(*) FROM "postgres/other".pg_catalog.pg_trigger WHERE NOT tgisinternal}),'2','another branch keeps its triggers');

# ---- not versioned ----
sql(q{CREATE TABLE h(id int) USING heap; CREATE FUNCTION h_upcase() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.id := NEW.id * 10; RETURN NEW; END$$});
fails(q{CREATE TRIGGER h_t BEFORE INSERT ON h FOR EACH ROW EXECUTE FUNCTION h_upcase()},qr/cannot make an unversioned object depend on a versioned one/,'a native table cannot use a versioned trigger function');
$node->stop;
done_testing();
