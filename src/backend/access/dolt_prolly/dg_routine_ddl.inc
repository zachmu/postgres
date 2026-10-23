/* Route native signature checks to immutable, branch-local pg_proc values. */
static bool routine_creating;

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
dg_catalog_create_routine(ParseState *pstate, Node *statement)
{
	CatalogView *previous;
	bool		pushed = false;

	if (!IsA(statement, CreateFunctionStmt) || !dg_catalog_active())
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	previous = CatalogViewSwitch(dg_catalog_session_view());
	routine_creating = true;
	PG_TRY();
	{
		CreateFunction(pstate, (CreateFunctionStmt *) statement);
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
		member = routine_member(definition);
		payload = routine_payload(definition);
		run("SELECT jsonb_set($1::jsonb,'{members}',COALESCE((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'members') v WHERE NOT (v->>'kind'='routine' AND v->>'name'=$2)),'[]'::jsonb)||jsonb_build_array($3::jsonb))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(name), CStringGetTextDatum(payload)), NULL, true);
		json = strcell(0, 1);
		run("SELECT o.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) h JOIN pg_dolt_object_visible o ON o.objhash=h WHERE d.objhash=$1 AND NOT (o.objkind='routine' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2)", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name)), NULL, true);
		n = SPI_processed;
		members = palloc_array(Datum, n + 1);
		for (int i = 0; i < n; i++)
			members[i] = cell(i, 1);
		members[n] = PointerGetDatum(member);
		database = canonical_database(json, members, n + 1);
		move_ref(branch_ref("working"), working, database);
		advance_snapshot();
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
