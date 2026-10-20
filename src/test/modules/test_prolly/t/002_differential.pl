use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('compare');
$node->init;
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\n");
$node->start;
$node->safe_psql('postgres',q{
CREATE TABLE p(id int PRIMARY KEY,v text COLLATE "C", n int) USING dolt_prolly_table;
CREATE TABLE h(id int PRIMARY KEY,v text COLLATE "C", n int);
INSERT INTO p SELECT g,'v'||g,CASE WHEN g%4=0 THEN NULL ELSE g%7 END FROM generate_series(1,40) g;
INSERT INTO h SELECT * FROM p;
CREATE INDEX pn ON p(n DESC NULLS LAST,v ASC) INCLUDE(id);
CREATE INDEX hn ON h(n DESC NULLS LAST,v ASC) INCLUDE(id);
});
for my $step (1..12) {
 my $id=($step*13)%40+1;
 my $sql="BEGIN; UPDATE p SET v=v||'x',n=COALESCE(n,0)+1 WHERE id=$id; UPDATE h SET v=v||'x',n=COALESCE(n,0)+1 WHERE id=$id;";
 if($step%3==0) { $sql.="DELETE FROM p WHERE id=$step; DELETE FROM h WHERE id=$step;"; }
 $sql.=($step%4==0?'ROLLBACK':'COMMIT');
 $node->safe_psql('postgres',$sql);
 for my $predicate ('id>=15 AND id<27','n IS NULL','n>=3','true') {
  my $query="SELECT id,v,n FROM %s WHERE $predicate ORDER BY n DESC NULLS LAST,v ASC,id";
  my $actual=$node->safe_psql('postgres','SET enable_seqscan=off; '.sprintf($query,'p'));
  my $expected=$node->safe_psql('postgres',sprintf($query,'h'));
  is($actual,$expected,"heap comparison step $step: $predicate");
 }
}
$node->safe_psql('postgres',q{
CREATE TABLE fresh(LIKE p) USING dolt_prolly_table;
INSERT INTO fresh SELECT * FROM h ORDER BY id DESC;
CREATE INDEX fresh_n ON fresh(n DESC NULLS LAST,v ASC) INCLUDE(id);
});
is($node->safe_psql('postgres',q{SELECT dolt_prolly_root('p')=dolt_prolly_root('fresh')}),'t','mixed histories produce fresh canonical table root');
is($node->safe_psql('postgres',q{SELECT dolt_prolly_root('pn')=dolt_prolly_root('fresh_n')}),'t','mixed histories produce fresh canonical index root');
my $reader=$node->background_psql('postgres',on_error_stop=>0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM p');
$node->safe_psql('postgres',q{INSERT INTO p VALUES(99,'new',1)});
my ($out,$err)=$reader->query(q{INSERT INTO p VALUES(100,'stale',2)});
is($err,1,'stale writer raises an error');
$reader->query('ROLLBACK');
$reader->quit;
is($node->safe_psql('postgres',q{SELECT count(*) FROM p WHERE id=100}),'0','stale writer does not publish a root');
$node->stop;
done_testing();
