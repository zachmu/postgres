use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('identifiers');
$node->init(extra => ['--locale=C', '--encoding=UTF8']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;

sub sql { $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE DOMAIN dv AS oidvector;
CREATE DOMAIN di AS int2vector;
CREATE TYPE wrapped AS (r regtype[], x xid, t tid);
CREATE TABLE explicit(id int PRIMARY KEY, r regtype, x xid, c cid, t tid, w wrapped, v dv);
INSERT INTO explicit VALUES(1,'int4','4294967295','4294967295','(123,4)',ROW(ARRAY['int4'::regtype,'text'::regtype],'42'::xid,'(9,2)'), '23 25')});
is(sql(q{SELECT typbasetype::regtype FROM pg_type WHERE typname='dv'}),'oidvector','domain preserves oidvector identity');
is(sql(q{SELECT typbasetype::regtype FROM pg_type WHERE typname='di'}),'int2vector','domain preserves int2vector identity');
is(sql(q{SELECT ARRAY[23::oid,25::oid]::dv}),'23 25','domain uses native vector output');
is(sql(q{SELECT r::oid,x,c,t,v FROM explicit}),'23|4294967295|4294967295|(123,4)|23 25','explicit identifier values round trip');
sql(q{SELECT dolt_commit('-Am','identifiers')});
sql(q{UPDATE explicit SET r='text',x='7',c='8',t='(99,1)',v='25 23'});
is(sql(q{SELECT r::oid,x,c,t,v FROM explicit AS OF 'HEAD'}),'23|4294967295|4294967295|(123,4)|23 25','history retains explicit numbers');
sql(q{CREATE TYPE a AS (r regtype[],x xid); CREATE TYPE b AS (r regtype[],x xid);
CREATE TABLE ta(v a); CREATE TABLE tb(v b);
INSERT INTO ta VALUES(ROW(ARRAY['int4'::regtype],'42'::xid)); INSERT INTO tb VALUES(ROW(ARRAY['int4'::regtype],'42'::xid))});
is(sql(q{SELECT dolt_prolly_root('ta')=dolt_prolly_root('tb')}),'t','incidental composite identity does not enter payload');
sql(q{UPDATE tb SET v=ROW(ARRAY['text'::regtype],'42'::xid)});
is(sql(q{SELECT dolt_prolly_root('ta')<>dolt_prolly_root('tb')}),'t','explicit regtype payload does enter content');
$node->restart;
is(sql(q{SELECT r::oid,x,c,t,v FROM explicit}),'25|7|8|(99,1)|25 23','identifiers and domain descriptor survive restart');
is(sql(q{SELECT (w).r::text,(w).x,(w).t FROM explicit AS OF 'HEAD'}),'{integer,text}|42|(9,2)','historical nested identifiers survive restart');
$node->stop;
done_testing();
