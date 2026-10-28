# DDL reads a branch's schema natively: the internal statements it runs do not
# depend on how many objects the schema holds. pg_stat_statements with
# track=all records the internal statements a DDL still runs.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('catalog_scaling');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\nshared_preload_libraries='pg_stat_statements'\npg_stat_statements.track='all'\n");
$node->start;
$node->safe_psql('postgres',q{CREATE EXTENSION pg_stat_statements});
my $s=$node->background_psql('postgres',on_error_stop=>0);

# Tables with rows, so that capturing the branch's state records their contents.
sub add_tables {
 my ($from,$to)=@_;
 for my $i ($from..$to) {
  $s->query_safe("CREATE TABLE t$i(id int PRIMARY KEY, v text, w int); CREATE INDEX t${i}_w ON t$i(w); INSERT INTO t$i VALUES (1,'one',1),(2,'two',2)");
 }
}

# The internal statement calls |ddl| makes, after a warm-up of the same kind.
sub internal_calls {
 my ($warmup,$ddl)=@_;
 $s->query_safe($warmup);
 $s->query_safe(q{SELECT pg_stat_statements_reset()});
 $s->query_safe($ddl);
 return $s->query_safe(q{SELECT coalesce(sum(calls),0) FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%'});
}

my %ddl=(
 'CREATE TABLE'=>['CREATE TABLE warm_%d(id int PRIMARY KEY, v text)','CREATE TABLE probe_%d(id int PRIMARY KEY, v text)'],
 'CREATE INDEX'=>['CREATE INDEX ON warm_%d(v)','CREATE INDEX ON probe_%d(v)'],
 'ALTER TABLE'=>['ALTER TABLE warm_%d ADD COLUMN x int','ALTER TABLE probe_%d ADD COLUMN x int'],
 'DROP TABLE'=>['DROP TABLE warm_%d','DROP TABLE probe_%d'],
);
my @order=('CREATE TABLE','CREATE INDEX','ALTER TABLE','DROP TABLE');

add_tables(1,4);
my %small;
$small{$_}=internal_calls(sprintf($ddl{$_}[0],1),sprintf($ddl{$_}[1],1)) for @order;
add_tables(5,40);
for my $kind (@order) {
 is(internal_calls(sprintf($ddl{$kind}[0],2),sprintf($ddl{$kind}[1],2)),$small{$kind},"$kind runs as many internal statements on 40 tables as on 4")
   or diag($s->query_safe(q{SELECT calls||' '||left(regexp_replace(query,'\s+',' ','g'),200) FROM pg_stat_statements WHERE query NOT LIKE '%pg_stat_statements%' ORDER BY calls DESC LIMIT 12}));
}
is($s->query_safe(q{SELECT count(*) FROM t40}),'2','tables keep their rows');

# A statement keeps the views its lookups return although many branch views
# exist: superseded ones are released only at transaction end.
$s->query_safe(q{create type two_ints as (a int, b int); create type two_ints_range as range (subtype = two_ints)});
my ($out,$failed)=$s->query(q{alter type two_ints add attribute c two_ints_range});
like($s->{stderr},qr/cannot be made a member of itself/,'a type lookup among many branch views reports the type error');
$s->{stderr}='';
is($s->query_safe(q{SELECT 1}),'1','the session survives the lookup');

# A session keeps a bounded number of catalog views loaded however much DDL it
# runs: each DDL's private view is freed, and superseded views are unloaded.
cmp_ok($s->query_safe(q{SELECT count(*) FROM pg_backend_memory_contexts WHERE name='catalog view rows' AND total_bytes > 65536}),'<=',32,'loaded catalog views stay bounded across DDL');

# An object's OID stays valid across later DDL, as psql's \d relies on.
my $type=$s->query_safe(q{CREATE TYPE kept AS (a int); SELECT 'kept'::regtype::oid});
add_tables(41,60);
is($s->query_safe("SELECT format_type($type, NULL)"),'kept','an OID from before later DDL still names its object');

$s->quit;
done_testing();
