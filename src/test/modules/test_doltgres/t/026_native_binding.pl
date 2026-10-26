# Query-time binding of versioned names runs no SQL of its own: a reference
# resolves in the revision's catalog view, as a name resolves in the physical
# catalogs. pg_stat_statements with track=all records the internal statements
# that binding still runs, so the test can see them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('native_binding');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\nshared_preload_libraries='pg_stat_statements'\npg_stat_statements.track='all'\ncompute_query_id=on\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sql(q{CREATE EXTENSION pg_stat_statements; CREATE ROLE reader IN ROLE pg_read_all_data; CREATE ROLE noperms; CREATE SCHEMA app; REVOKE USAGE ON SCHEMA app FROM PUBLIC; CREATE TABLE t(id int PRIMARY KEY, v text); INSERT INTO t SELECT g, 'v'||g FROM generate_series(1,100) g; CREATE INDEX t_v ON t(v); CREATE TABLE app.t(id int PRIMARY KEY, v text); INSERT INTO app.t VALUES(1,'app'); CREATE TABLE h(id int PRIMARY KEY, v text) USING heap; INSERT INTO h VALUES(1,'heap'); CREATE VIEW tv AS SELECT v FROM t WHERE id=2;  SELECT dolt_commit('-Am','base')});

# The internal statements one session runs for |query|, after its caches are warm.
my $s=$node->background_psql('postgres');
sub internal {
 my ($query)=@_;
 $s->query_safe($query);
 $s->query_safe(q{SELECT pg_stat_statements_reset()});
 $s->query_safe($query);
 my $rows=$s->query_safe(q{SELECT regexp_replace(query,'\s+',' ','g') FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%' ORDER BY 1});
 return grep {length} split /\n/, $rows;
}
sub no_lookup_sql {
 my ($query,$label)=@_;
 my @stmts=internal($query);
 my @lookups=grep {/unnest\(d\.objchildren\)|jsonb|pg_dolt_binding/} @stmts;
 is(scalar @lookups,0,"$label resolves without object-store SQL") or diag(join("\n",@stmts));
 return @stmts;
}
no_lookup_sql(q{SELECT v FROM t WHERE id=1},'branch table');
no_lookup_sql(q{SELECT v FROM app.t WHERE id=1},'qualified branch table');
is($s->query_safe(q{SELECT v FROM t AS OF 'HEAD' WHERE id=1}),'v1','revision table');
no_lookup_sql(q{SELECT v FROM tv},'versioned view');
no_lookup_sql(q{SELECT v FROM h WHERE id=1},'heap table beside branch tables');
no_lookup_sql(q{SELECT 't'::regclass::oid > 0},'regclass lookup');
no_lookup_sql(q{UPDATE t SET v=v WHERE id=1},'write target');
is($s->query_safe(q{SET search_path=app,public; SELECT v FROM t WHERE id=1}),'app','search_path order selects the namespace');
$s->query_safe(q{RESET search_path});

# Permissions come from the versioned definitions.
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}
is(sql(q{SET ROLE reader; SELECT v FROM t WHERE id=3}),'v3','predefined reader role reads the branch table');
is(sql(q{SET ROLE reader; SELECT v FROM app.t}),'app','predefined reader role uses an unusable namespace, as in PostgreSQL');
is(sql(q{SET ROLE reader; SELECT v FROM t AS OF 'HEAD' WHERE id=3}),'v3','predefined reader role reads the revision');
fails(q{SET ROLE noperms; SELECT v FROM t WHERE id=3},qr/permission denied for table t/,'unprivileged reader is refused');
fails(q{SET ROLE noperms; SELECT v FROM app.t},qr/permission denied for schema app/,'unprivileged reader is refused the namespace');
fails(q{SET ROLE noperms; SET search_path=app,public; SELECT v FROM t},qr/permission denied for table t/,'an unusable namespace is skipped, as in PostgreSQL');
sql(q{REVOKE pg_read_all_data FROM reader});
fails(q{SET ROLE reader; SELECT v FROM t WHERE id=3},qr/permission denied for table t/,'revocation applies to the next reference');
fails(q{SELECT * FROM t_v},qr/relation t_v does not exist/,'an index is not a table');
fails(q{SELECT * FROM missing},qr/relation missing does not exist on branch main/,'missing relation');

# Schema changes and branch switches still rebind.
sql(q{ALTER TABLE t ADD COLUMN w int});
is($s->query_safe(q{SELECT w IS NULL FROM t WHERE id=1}),'t','ALTER is visible to the warm session');
$s->query_safe(q{SELECT dolt_checkout('-b','other'); UPDATE t SET v='other' WHERE id=1});
is($s->query_safe(q{SELECT v FROM t WHERE id=1}),'other','branch write is visible on the branch');
is(sql(q{SELECT v FROM t WHERE id=1}),'v1','main is unchanged');
$s->quit;
$node->stop;
done_testing();
