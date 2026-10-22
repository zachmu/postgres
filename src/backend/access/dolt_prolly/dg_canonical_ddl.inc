/* Canonical DDL publishes definitions and local resources, never schema copies. */
static char *
canonical_definition(const char *name, Oid owner, Acl *acl, const char *columns,
                     const char *indexes, const char *constraints, const char *indexdef)
{
 run("SELECT jsonb_build_object('format','DGTABLE1','name',$1::text,'owner',pg_get_userbyid($2),'comment',NULL,'acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode($3::aclitem[]) x),'[]'::jsonb),'columns',$4::jsonb,'indexes',$5::jsonb,'constraints',$6::jsonb,'index_definition',NULLIF($7::text,''))::text",7,VC_TYPES(TEXTOID,OIDOID,ACLITEMARRAYOID,TEXTOID,TEXTOID,TEXTOID,TEXTOID),VC_ARGS(CStringGetTextDatum(name),ObjectIdGetDatum(owner),PointerGetDatum(acl),CStringGetTextDatum(columns),CStringGetTextDatum(indexes),CStringGetTextDatum(constraints),CStringGetTextDatum(indexdef ? indexdef : "")),NULL,true);
 return strcell(0,1);
}

static bytea *
canonical_empty_member(const char *kind, const char *name, const char *definition, PLTree *empty)
{
 bytea *schema=put("schema",definition,NULL,0);
 bytea *content=dg_history_export_tree(empty);
 bytea *raw=palloc(VARHDRSZ+PL_HASH_SIZE);
 char *payload;
 SET_VARSIZE(raw,VARHDRSZ+PL_HASH_SIZE);
 memcpy(VARDATA(raw),empty->root.bytes,PL_HASH_SIZE);
 run("SELECT jsonb_build_object('kind',$1::text,'name',$2::text,'schema',encode(sha256(convert_to($3,'UTF8')),'hex'),'content',$4::text)::text",4,VC_TYPES(TEXTOID,TEXTOID,TEXTOID,TEXTOID),VC_ARGS(CStringGetTextDatum(kind),CStringGetTextDatum(name),CStringGetTextDatum(definition),CStringGetTextDatum(hex(raw))),NULL,true);
 payload=strcell(0,1);
 return put(kind,payload,VC_ARGS(PointerGetDatum(schema),PointerGetDatum(content)),2);
}

bool
dg_catalog_create(Node *statement, const char *query)
{
 CreateStmt *stmt;
 Oid user, namespaceid, existing;
 int security,nest;
 bool pushed=false,skip=false;
 char *name;
 List *commands,*indexcmds=NIL;
 ListCell *lc;
 TupleDesc desc;
 Acl *acl;

 if (!IsA(statement,CreateStmt) || !dg_catalog_active()) return false;
 stmt=copyObject((CreateStmt *)statement);
 if (XactReadOnly) PreventCommandIfReadOnly("CREATE TABLE");
 if (stmt->relation->relpersistence!=RELPERSISTENCE_PERMANENT || stmt->inhRelations || stmt->partbound || stmt->partspec || stmt->ofTypename || stmt->options || stmt->tablespacename || (stmt->accessMethod && strcmp(stmt->accessMethod,"dolt_prolly_table")))
  unsupported("branch CREATE requires a plain permanent prolly table");
 if (!ActiveSnapshotSet()) { PushActiveSnapshot(GetTransactionSnapshot());pushed=true; }
 GetUserIdAndSecContext(&user,&security);
 namespaceid=RangeVarGetAndCheckCreationNamespace(stmt->relation,NoLock,&existing);
 stmt->relation->schemaname=get_namespace_name(namespaceid);
 name=quote_qualified_identifier(stmt->relation->schemaname,stmt->relation->relname);
 dg_storage_lock_working(dg_current_branch(),GetActiveSnapshot());
 SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,security|SECURITY_LOCAL_USERID_CHANGE);
 PG_TRY();
 {
  nest=begin_api();
  catalog_acl(catalog_namespace(read_ref(branch_ref("working")).target,stmt->relation->schemaname),"CREATE",user);
  skip=catalog_contains(read_ref(branch_ref("working")).target,name);
  if (skip && !stmt->if_not_exists)
   ereport(ERROR,(errcode(ERRCODE_DUPLICATE_TABLE),errmsg("relation \"%s\" already exists",stmt->relation->relname)));
  run("SELECT 1 FROM pg_event_trigger WHERE evtenabled <> 'D'",0,NULL,NULL,NULL,true);
  if (SPI_processed) unsupported("branch CREATE with event triggers");
  end_api(nest);
 }
 PG_FINALLY(); { SetUserIdAndSecContext(user,security); } PG_END_TRY();
 if (skip)
 {
  ereport(NOTICE,(errmsg("relation \"%s\" already exists, skipping",stmt->relation->relname)));
  if(pushed)PopActiveSnapshot();
  return true;
 }
 stmt->if_not_exists=false;
 stmt->accessMethod=pstrdup("dolt_prolly_table");
 commands=transformCreateStmt(stmt,query);
 foreach(lc,commands)
 {
  Node *command=lfirst(lc);
  if (IsA(command,CreateStmt)) stmt=(CreateStmt *)command;
  else if (IsA(command,IndexStmt)) indexcmds=lappend(indexcmds,command);
  else unsupported("canonical CREATE auxiliary command");
 }
 foreach(lc,stmt->tableElts)
 {
  ColumnDef *col=lfirst_node(ColumnDef,lc);
  if(col->raw_default || col->cooked_default || col->identity || col->generated)
   unsupported("canonical CREATE defaults or generated columns");
 }
 foreach(lc,stmt->constraints)
 {
  Constraint *constraint=lfirst_node(Constraint,lc);
  if(constraint->contype!=CONSTR_NOTNULL) unsupported("canonical CREATE constraint");
 }
 desc=BuildDescForRelation(stmt->tableElts);
 dg_validate_descriptor(desc);
 for(int i=0;i<desc->natts;i++)
  for(int j=0;j<i;j++)
   if(!strcmp(NameStr(TupleDescAttr(desc,i)->attname),NameStr(TupleDescAttr(desc,j)->attname)))
    ereport(ERROR,(errcode(ERRCODE_DUPLICATE_COLUMN),errmsg("column \"%s\" specified more than once",NameStr(TupleDescAttr(desc,i)->attname))));
 acl=get_user_default_acl(OBJECT_TABLE,user,namespaceid);
 if(!acl)acl=acldefault(OBJECT_TABLE,user);
 SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,security|SECURITY_LOCAL_USERID_CHANGE);
 PG_TRY();
 {
  VersionRef working;
  Capture current;
  StringInfoData columns,indexes,constraints,added;
  char **column_json=palloc_array(char *,Max(desc->natts,1));
  char **index_defs=palloc_array(char *,Max(list_length(indexcmds),1));
  char **index_names=palloc_array(char *,Max(list_length(indexcmds),1));
  char **index_columns=palloc_array(char *,Max(list_length(indexcmds),1));
  bytea **newmembers=palloc_array(bytea *,list_length(indexcmds)+1);
  Datum *members;
  int oldcount,k=0,total;
  bytea *database;
  CatalogView *view;
  DGState *empty=palloc0(sizeof(*empty));

  nest=begin_api();
  working=read_ref(branch_ref("working"));
  current=capture(true);
  initStringInfo(&columns);appendStringInfoChar(&columns,'[');
  initStringInfo(&indexes);appendStringInfoChar(&indexes,'[');
  initStringInfo(&constraints);appendStringInfoChar(&constraints,'[');
  for(int i=0;i<desc->natts;i++)
  {
   Form_pg_attribute a=TupleDescAttr(desc,i);
   if(i)appendStringInfoChar(&columns,',');
   run("SELECT jsonb_build_object('name',$1::text,'comment',NULL,'type',t.typname,'typmod',$3::int,'notnull',$4::boolean,'collation',CASE WHEN $5::oid=0 THEN NULL ELSE (SELECT jsonb_build_array(n.nspname,c.collname,c.collprovider,c.collisdeterministic,c.collcollate,c.collctype,c.colllocale,c.collversion) FROM pg_collation c JOIN pg_namespace n ON n.oid=c.collnamespace WHERE c.oid=$5) END)::text FROM pg_type t WHERE t.oid=$2 AND t.typnamespace='pg_catalog'::regnamespace",5,VC_TYPES(TEXTOID,OIDOID,INT4OID,BOOLOID,OIDOID),VC_ARGS(CStringGetTextDatum(NameStr(a->attname)),ObjectIdGetDatum(a->atttypid),Int32GetDatum(a->atttypmod),BoolGetDatum(a->attnotnull),ObjectIdGetDatum(a->attcollation)),NULL,true);
   if(SPI_processed!=1)unsupported("canonical CREATE user-defined column types");
   column_json[i]=strcell(0,1);appendStringInfoString(&columns,column_json[i]);
   if(a->attnotnull)
   {
    if(constraints.len>1)appendStringInfoChar(&constraints,',');
    appendStringInfoChar(&constraints,'[');escape_json(&constraints,makeObjectName(stmt->relation->relname,NameStr(a->attname),"not_null"));appendStringInfoChar(&constraints,',');escape_json(&constraints,psprintf("NOT NULL %s",quote_identifier(NameStr(a->attname))));appendStringInfoChar(&constraints,']');
   }
  }
  appendStringInfoChar(&columns,']');
  foreach(lc,indexcmds)
  {
   IndexStmt *index=lfirst_node(IndexStmt,lc);
   StringInfoData keys,icols;
   ListCell *key;
   char *indexname,*sql,*constraint;
   if(index->deferrable || index->whereClause || index->indexIncludingParams || index->options || index->excludeOpNames || !index->unique)
    unsupported("canonical CREATE index properties");
   initStringInfo(&keys);initStringInfo(&icols);appendStringInfoChar(&icols,'[');
   foreach(key,index->indexParams)
   {
    IndexElem *element=lfirst_node(IndexElem,key);
    int att;
    if(!element->name || element->expr || element->opclass || element->collation || element->ordering!=SORTBY_DEFAULT || element->nulls_ordering!=SORTBY_NULLS_DEFAULT)
     unsupported("canonical CREATE index expressions/options");
    att=provider_attribute(desc,element->name)-1;
    if(keys.len){appendStringInfoString(&keys,", ");appendStringInfoChar(&icols,',');}
    appendStringInfoString(&keys,quote_identifier(element->name));appendStringInfoString(&icols,column_json[att]);
   }
   appendStringInfoChar(&icols,']');
   indexname=index->idxname ? index->idxname : makeObjectName(stmt->relation->relname,index->primary ? NULL : ((IndexElem *)linitial(index->indexParams))->name,index->primary ? "pkey" : "key");
   if(catalog_contains(working.target,quote_qualified_identifier(stmt->relation->schemaname,indexname)))
    ereport(ERROR,(errcode(ERRCODE_DUPLICATE_TABLE),errmsg("relation \"%s\" already exists",indexname)));
   for(int j=0;j<k;j++)if(!strcmp(index_names[j],quote_qualified_identifier(stmt->relation->schemaname,indexname)))
    ereport(ERROR,(errcode(ERRCODE_DUPLICATE_OBJECT),errmsg("duplicate canonical index name")));
   sql=psprintf("CREATE UNIQUE INDEX %s ON %s USING dolt_prolly_index (%s)%s",quote_identifier(indexname),name,keys.data,index->nulls_not_distinct ? " NULLS NOT DISTINCT" : "");
   constraint=psprintf("%s%s (%s)",index->primary ? "PRIMARY KEY" : "UNIQUE",(!index->primary && index->nulls_not_distinct) ? " NULLS NOT DISTINCT" : "",keys.data);
   if(k)appendStringInfoChar(&indexes,',');escape_json(&indexes,sql);
   if(constraints.len>1)appendStringInfoChar(&constraints,',');
   appendStringInfoChar(&constraints,'[');escape_json(&constraints,indexname);appendStringInfoChar(&constraints,',');escape_json(&constraints,constraint);appendStringInfoChar(&constraints,']');
   index_names[k]=quote_qualified_identifier(stmt->relation->schemaname,indexname);
   index_columns[k]=icols.data;index_defs[k++]=sql;
  }
  appendStringInfoChar(&indexes,']');appendStringInfoChar(&constraints,']');
  empty->store=pl_store_create();
  dg_check(pl_build(empty->store,NULL,0,&empty->content));
  newmembers[0]=canonical_empty_member("table",name,canonical_definition(name,user,acl,columns.data,indexes.data,constraints.data,NULL),&empty->content);
  for(int i=0;i<k;i++)
   newmembers[i+1]=canonical_empty_member("index",index_names[i],canonical_definition(index_names[i],user,acldefault(OBJECT_TABLE,user),index_columns[i],"[]","[]",index_defs[i]),&empty->content);
  dg_release(empty);
  run("SELECT unnest(objchildren) FROM pg_dolt_object WHERE objhash=$1",1,VC_TYPES(BYTEAOID),VC_ARGS(PointerGetDatum(current.hash)),NULL,true);
  oldcount=SPI_processed;total=oldcount+k+1;members=palloc_array(Datum,total);
  for(int i=0;i<oldcount;i++)members[i]=cell(i,1);
  initStringInfo(&added);appendStringInfoChar(&added,'[');
  for(int i=0;i<=k;i++)
  {
   members[oldcount+i]=PointerGetDatum(newmembers[i]);
   if(i)appendStringInfoChar(&added,',');
   appendStringInfoString(&added,metadata(newmembers[i],i ? "index" : "table"));
  }
  appendStringInfoChar(&added,']');
  run("SELECT jsonb_set($1::jsonb,'{members}',($1::jsonb->'members')||$2::jsonb)::text",2,VC_TYPES(TEXTOID,TEXTOID),VC_ARGS(CStringGetTextDatum(current.json),CStringGetTextDatum(added.data)),NULL,true);
  database=canonical_database(strcell(0,1),members,total);
  view=provider_view(database);
  for(int i=0;i<=k;i++)
  {
   const char *membername=i ? index_names[i-1] : name;
   Relation definition=relation_open(CatalogViewLookupObject(view,RelationRelationId,membername),NoLock);
   Oid resource=dg_storage_allocate(definition,dg_current_branch());
   binding_record(dg_current_branch(),membername,child(newmembers[i],1),resource);
   relation_close(definition,NoLock);
  }
  move_ref(branch_ref("working"),working,database);
  advance_snapshot();
  {
   Relation rel=dg_catalog_write_table(dg_current_branch(),name,ACL_INSERT);
   dg_rebuild_indexes(rel);
   dg_storage_close_relation(rel);
  }
  end_api(nest);
 }
 PG_FINALLY();
 {
  SetUserIdAndSecContext(user,security);
  if(pushed)PopActiveSnapshot();
 }
 PG_END_TRY();
 return true;
}
