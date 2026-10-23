/* Route native signature checks to immutable, branch-local pg_proc values. */
static bool routine_creating;
static Oid	routine_store(HeapTuple tuple, TupleDesc desc, bool remove);
static void routine_drop(DropStmt *stmt);

bool
dg_catalog_creating_routine(void)
{
	return routine_creating;
}

Oid
dg_catalog_routine_namespace(Oid namespaceid)
{
	if (!routine_creating)
		return namespaceid;
	namespaceid = CatalogViewNamespace(CatalogViewCurrent(), namespaceid);
	if (!CatalogViewObjectContext(namespaceid))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
						errmsg("routine target schema is not present in the selected revision")));
	return namespaceid;
}

bool
dg_catalog_routine_ddl(ParseState *pstate, Node *statement)
{
	CatalogView *previous;
	bool		pushed = false;

	if ((!IsA(statement, CreateFunctionStmt) &&
		 !(IsA(statement, DropStmt) &&
		   (((DropStmt *) statement)->removeType == OBJECT_FUNCTION ||
			((DropStmt *) statement)->removeType == OBJECT_ROUTINE))) || !dg_catalog_active())
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("routine DDL in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	{
		Oid			user;
		int			security,
					nest;

		GetUserIdAndSecContext(&user, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		PG_TRY();
		{
			nest = begin_api();
			run("SELECT 1 FROM pg_event_trigger WHERE evtenabled<>'D'", 0, NULL, NULL, NULL, true);
			if (SPI_processed)
				unsupported("canonical routine DDL with event triggers");
			end_api(nest);
		}
		PG_FINALLY();
		{
			SetUserIdAndSecContext(user, security);
		}
		PG_END_TRY();
	}
	previous = CatalogViewSwitch(dg_catalog_session_view());
	routine_creating = IsA(statement, CreateFunctionStmt);
	PG_TRY();
	{
		if (routine_creating)
			CreateFunction(pstate, (CreateFunctionStmt *) statement);
		else
			routine_drop((DropStmt *) statement);
	}
	PG_FINALLY();
	{
		routine_creating = false;
		CatalogViewSwitch(previous);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}

Oid
dg_catalog_store_routine(HeapTuple tuple, TupleDesc desc)
{
	return routine_store(tuple, desc, false);
}

static Oid
routine_store(HeapTuple tuple, TupleDesc desc, bool remove)
{
	Oid			user,
				result = InvalidOid;
	int			security,
				nest;
	CatalogView *previous = CatalogViewCurrent();

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		char	   *definition,
				   *payload,
				   *name,
				   *json;
		bytea	   *member,
				   *database;
		VersionRef	working;
		Capture		current;
		Datum	   *members;
		int			n;

		nest = begin_api();
		definition = routine_encode(tuple, desc);
		name = json_value(definition, "name");
		CatalogViewSwitch(NULL);
		working = read_ref(branch_ref("working"));
		current = capture(true);
		member = remove ? NULL : routine_member(definition);
		payload = remove ? pstrdup("[]") : psprintf("[%s]", routine_payload(definition));
		run("SELECT jsonb_set($1::jsonb,'{members}',COALESCE((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'members') v WHERE NOT (v->>'kind'='routine' AND v->>'name'=$2)),'[]'::jsonb)||$3::jsonb)::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(name), CStringGetTextDatum(payload)), NULL, true);
		json = strcell(0, 1);
		run("SELECT o.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) h JOIN pg_dolt_object_visible o ON o.objhash=h WHERE d.objhash=$1 AND NOT (o.objkind='routine' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2)", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name)), NULL, true);
		n = SPI_processed;
		members = palloc_array(Datum, n + 1);
		for (int i = 0; i < n; i++)
			members[i] = cell(i, 1);
		if (!remove)
			members[n++] = PointerGetDatum(member);
		database = canonical_database(json, members, n);
		move_ref(branch_ref("working"), working, database);
		advance_snapshot();
		if (!remove)
			result = CatalogViewLookupObject(provider_view(database), ProcedureRelationId, name);
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

static void
routine_drop(DropStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->objects)
	{
		ObjectWithArgs *object = lfirst_node(ObjectWithArgs, lc);
		Oid			id = LookupFuncWithArgs(stmt->removeType, object, stmt->missing_ok);
		HeapTuple	tuple;
		Relation	catalog,
					depend;
		SysScanDesc scan;
		ScanKeyData keys[2];

		if (!OidIsValid(id))
			continue;
		if (!CatalogViewObjectContext(id))
			unsupported("DROP of a shared routine in an activated database");
		if (!object_ownercheck(ProcedureRelationId, id, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, get_func_name(id));
		depend = table_open(DependRelationId, AccessShareLock);
		ScanKeyInit(&keys[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(ProcedureRelationId));
		ScanKeyInit(&keys[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(id));
		scan = systable_beginscan(depend, DependReferenceIndexId, true, NULL, 2, keys);
		if (HeapTupleIsValid(systable_getnext(scan)))
			ereport(ERROR, (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST), errmsg("cannot drop function because other objects depend on it"), errdetail("Canonical dependent-object deletion is required before this routine can be removed.")));
		systable_endscan(scan);
		table_close(depend, AccessShareLock);
		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(id));
		catalog = table_open(ProcedureRelationId, AccessShareLock);
		(void) routine_store(tuple, RelationGetDescr(catalog), true);
		table_close(catalog, AccessShareLock);
		ReleaseSysCache(tuple);
	}
}
