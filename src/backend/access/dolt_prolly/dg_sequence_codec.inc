/* Sequence definitions and snapshots are portable; allocation is local and shared. */
static Oid
sequence_resource(const char *name, int64 start, int64 increment, bool create)
{
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Oid			result;

	if (list_length(parts) != 2)
		elog(ERROR, "sequence identity is not qualified");
	if (create)
	{
		LockDatabaseObject(DoltSequenceRelationId, hash_bytes((const unsigned char *) name, strlen(name)), 0, ExclusiveLock);
		PushActiveSnapshot(GetLatestSnapshot());
	}
	run("SELECT seqstorage FROM pg_dolt_sequence WHERE seqnamespace=$1::name AND seqname=$2::name", 2,
		VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(strVal(linitial(parts))), CStringGetTextDatum(strVal(lsecond(parts)))), NULL, true);
	if (create)
		PopActiveSnapshot();
	if (SPI_processed)
		return DatumGetObjectId(cell(0, 1));
	if (!create)
		return InvalidOid;
	result = dg_storage_allocate_kind(RELKIND_SEQUENCE, NULL);
	{
		RelationData rel = {0};
		FormData_pg_class form = {0};

		form.relkind = RELKIND_SEQUENCE;
		form.relpersistence = RELPERSISTENCE_PERMANENT;
		rel.rd_rel = &form;
		rel.rd_id = result;
		rel.rd_backend = INVALID_PROC_NUMBER;
		rel.rd_lockInfo.lockRelId.relId = result;
		rel.rd_lockInfo.lockRelId.dbId = MyDatabaseId;
		dg_storage_locator(result, &rel.rd_locator);
		SequenceInitializeResource(&rel, start);
		RelationCloseSmgr(&rel);
	}
	run("INSERT INTO pg_dolt_sequence VALUES($1::name,$2::name,$3,$4)", 4,
		VC_TYPES(TEXTOID, TEXTOID, OIDOID, INT8OID), VC_ARGS(CStringGetTextDatum(strVal(linitial(parts))), CStringGetTextDatum(strVal(lsecond(parts))), ObjectIdGetDatum(result), Int64GetDatum(increment)), NULL, false);
	CommandCounterIncrement();
	return result;
}

static char *
sequence_encode(Oid id)
{
	HeapTuple	tuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(id));
	HeapTuple	classtuple = SearchSysCache1(RELOID, ObjectIdGetDatum(id));
	bool		aclnull;
	Datum		aclvalue;
	Acl		   *acl;
	Form_pg_sequence seq;
	Relation	rel;
	StringInfoData json;
	char	   *name;

	if (!HeapTupleIsValid(tuple) || !HeapTupleIsValid(classtuple))
		elog(ERROR, "sequence definition is missing");
	aclvalue = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_relacl, &aclnull);
	acl = aclnull ? acldefault(OBJECT_SEQUENCE, ((Form_pg_class) GETSTRUCT(classtuple))->relowner) : DatumGetAclPCopy(aclvalue);
	seq = (Form_pg_sequence) GETSTRUCT(tuple);
	rel = relation_open(id, NoLock);
	name = dg_qualified_name(get_namespace_name(rel->rd_rel->relnamespace), RelationGetRelationName(rel));
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGSEQ1\",\"name\":");
	escape_json(&json, name);
	appendStringInfoString(&json, ",\"owner\":");
	escape_json(&json, GetUserNameFromId(rel->rd_rel->relowner, false));
	appendStringInfo(&json, ",\"type\":\"%s\",\"start\":%" PRId64 ",\"increment\":%" PRId64 ",\"min\":%" PRId64 ",\"max\":%" PRId64 ",\"cache\":%" PRId64 ",\"cycle\":%s,\"last_value\":%" PRId64 ",\"is_called\":false,\"acl\":[]}",
					 seq->seqtypid == INT2OID ? "int2" : seq->seqtypid == INT4OID ? "int4" : "int8", seq->seqstart, seq->seqincrement, seq->seqmin, seq->seqmax, seq->seqcache, seq->seqcycle ? "true" : "false", seq->seqstart);
	json.len--;
	json.data[json.len] = '\0';
	{
		Relation	deps = table_open(DependRelationId, AccessShareLock);
		SysScanDesc scan = systable_beginscan(deps, InvalidOid, false, NULL, 0, NULL);
		HeapTuple	dep;

		while (HeapTupleIsValid(dep = systable_getnext(scan)))
		{
			Form_pg_depend edge = (Form_pg_depend) GETSTRUCT(dep);

			if (edge->classid == RelationRelationId && edge->objid == id && edge->refclassid == RelationRelationId && edge->refobjsubid > 0 &&
				(edge->deptype == DEPENDENCY_AUTO || edge->deptype == DEPENDENCY_INTERNAL))
			{
				appendStringInfoString(&json, ",\"owned_table\":");
				escape_json(&json, dg_qualified_name(get_namespace_name(get_rel_namespace(edge->refobjid)), get_rel_name(edge->refobjid)));
				appendStringInfoString(&json, ",\"owned_column\":");
				escape_json(&json, get_attname(edge->refobjid, edge->refobjsubid, false));
				appendStringInfo(&json, ",\"identity\":%s", edge->deptype == DEPENDENCY_INTERNAL ? "true" : "false");
			}
		}
		systable_endscan(scan);
		table_close(deps, AccessShareLock);
	}
	appendStringInfoChar(&json, '}');
	relation_close(rel, NoLock);
	ReleaseSysCache(tuple);
	ReleaseSysCache(classtuple);
	run("SELECT ($1::jsonb || jsonb_build_object('acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode($2::aclitem[]) x),'[]'::jsonb)))::text", 2, VC_TYPES(TEXTOID, ACLITEMARRAYOID), VC_ARGS(CStringGetTextDatum(json.data), PointerGetDatum(acl)), NULL, true);
	return strcell(0, 1);
}

static int64
sequence_number(const char *definition, const char *field)
{
	return pg_strtoint64(json_value(definition, field));
}

static char *
sequence_capture(const char *definition)
{
	Oid			resource = sequence_resource(json_value(definition, "name"), 0, 0, false);
	RelationData rel = {0};
	FormData_pg_class form = {0};
	int64		last;
	bool		called;

	if (!OidIsValid(resource))
		return (char *) definition;
	form.relkind = RELKIND_SEQUENCE;
	form.relpersistence = RELPERSISTENCE_PERMANENT;
	rel.rd_rel = &form;
	rel.rd_id = resource;
	rel.rd_backend = INVALID_PROC_NUMBER;
	dg_storage_locator(resource, &rel.rd_locator);
	SequenceReadResource(&rel, &last, &called);
	RelationCloseSmgr(&rel);
	run("SELECT ($1::jsonb || jsonb_build_object('last_value',$2::bigint,'is_called',$3::boolean))::text", 3,
		VC_TYPES(TEXTOID, INT8OID, BOOLOID), VC_ARGS(CStringGetTextDatum(definition), Int64GetDatum(last), BoolGetDatum(called)), NULL, true);
	return strcell(0, 1);
}

/* Allocation is shared, but a retained revision is never a write capability. */
void
dg_catalog_sequence_check(Relation rel)
{
	HeapTuple	tuple;
	Form_pg_sequence seq;
	int64		increment;
	const char *name;
	Oid			caller;
	int			security,
				nest;

	if (!rel->rd_catalogview)
		return;
	if (!CatalogViewIsDurable(RelationGetRelid(rel)))
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot advance a sequence in an immutable revision")));
	name = CatalogViewObjectIdentity(RelationGetRelid(rel), RelationRelationId);
	tuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(RelationGetRelid(rel)));
	seq = (Form_pg_sequence) GETSTRUCT(tuple);
	increment = seq->seqincrement;
	ReleaseSysCache(tuple);
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		List	   *parts = stringToQualifiedNameList(name, NULL);

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		run("SELECT seqincrement FROM pg_dolt_sequence WHERE seqnamespace=$1::name AND seqname=$2::name", 2,
			VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(strVal(linitial(parts))), CStringGetTextDatum(strVal(lsecond(parts)))), NULL, true);
		if (SPI_processed != 1 || ((DatumGetInt64(cell(0, 1)) > 0) != (increment > 0)))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unable to advance sequence state, possibly due to having incompatible state on different branches")));
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(caller, security);
	}
	PG_END_TRY();
}

static List *
sequence_definitions(CatalogView *view)
{
	List	   *definitions = NIL;
	CatalogView *previous = CatalogViewSwitch(view);

	foreach_ptr(HeapTupleData, tuple, CatalogViewTuples(view, SequenceRelationId))
	{
		Oid			id = ((Form_pg_sequence) GETSTRUCT(tuple))->seqrelid;

		if (id >= FirstNormalObjectId)
			definitions = lappend(definitions, sequence_encode(id));
	}
	CatalogViewSwitch(previous);
	return definitions;
}

/* Retained state is data, never a capability to mutate the shared allocator. */
bool
dg_catalog_sequence_retained(Relation rel, int64 *last, bool *called)
{
	Oid			caller;
	int			security,
				nest;

	if (!CatalogViewIsLeased(RelationGetRelid(rel)))
		return false;
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		char	   *definition;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		definition = metadata(CatalogViewObjectSchema(RelationGetRelid(rel)), "schema");
		*last = sequence_number(definition, "last_value");
		*called = !strcmp(json_value(definition, "is_called"), "true");
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(caller, security);
	}
	PG_END_TRY();
	return true;
}
