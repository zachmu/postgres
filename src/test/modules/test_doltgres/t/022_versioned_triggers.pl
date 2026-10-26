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
fails(q{ALTER TABLE t DROP COLUMN v},qr/trigger t_bump on table (public\.)?t depends on column v/,'trigger columns cannot be dropped alone');
fails(q{DROP FUNCTION bump()},qr/trigger t_bump on table t depends on function bump/,'a trigger function cannot be dropped alone');

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

# ---- drops ----
sql(q{DROP FUNCTION bump() CASCADE});
is(sql(q{SELECT count(*) FROM pg_trigger WHERE tgname='t_bump'}),'0','DROP FUNCTION CASCADE removes its triggers');
sql(q{DROP TABLE t});
is(sql(q{SELECT count(*) FROM pg_trigger WHERE NOT tgisinternal}),'0','DROP TABLE removes its triggers');
is(sql(q{SELECT count(*) FROM "postgres/other".pg_catalog.pg_trigger WHERE NOT tgisinternal}),'2','another branch keeps its triggers');

# ---- not versioned ----
sql(q{CREATE TABLE h(id int) USING heap; CREATE FUNCTION h_upcase() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.id := NEW.id * 10; RETURN NEW; END$$});
fails(q{CREATE TRIGGER h_t BEFORE INSERT ON h FOR EACH ROW EXECUTE FUNCTION h_upcase()},qr/cannot make an unversioned object depend on a versioned one/,'a native table cannot use a versioned trigger function');
sql(q{CREATE TABLE c(id int PRIMARY KEY)});
fails(q{CREATE CONSTRAINT TRIGGER c_t AFTER INSERT ON c FOR EACH ROW EXECUTE FUNCTION note()},qr/versioned constraint triggers/,'constraint triggers are rejected');
$node->stop;
done_testing();
