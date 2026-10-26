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

/*
 * Whether CREATE FUNCTION makes a versioned routine: SQL or PL/pgSQL in a user
 * schema. Others, such as C functions, stay native and unversioned.
 */
static bool
creates_versioned_routine(CreateFunctionStmt *stmt)
{
	char	   *name;
	Oid			namespace = QualifiedNameGetCreationNamespace(stmt->funcname, &name);
	const char *language = "sql";
	ListCell   *lc;

	if (isAnyTempNamespace(namespace) || dg_system_namespace(get_namespace_name(namespace)))
		return false;
	foreach(lc, stmt->options)
	{
		DefElem    *option = lfirst_node(DefElem, lc);

		if (!strcmp(option->defname, "language"))
			language = strVal(option->arg);
	}
	return !strcmp(language, "sql") || !strcmp(language, "plpgsql");
}

bool
dg_catalog_routine_ddl(ParseState *pstate, Node *statement)
{
	CatalogView *previous;
	bool		pushed = false;

	if (!IsA(statement, CreateFunctionStmt) || !dg_catalog_active() ||
		!creates_versioned_routine((CreateFunctionStmt *) statement))
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
		payload = psprintf("[%s]", routine_payload(definition));
		run("SELECT jsonb_set($1::jsonb,'{members}',COALESCE((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'members') v WHERE NOT (v->>'kind'='routine' AND v->>'name'=$2)),'[]'::jsonb)||$3::jsonb)::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(name), CStringGetTextDatum(payload)), NULL, true);
		json = strcell(0, 1);
		run("SELECT o.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) h JOIN pg_dolt_object_visible o ON o.objhash=h WHERE d.objhash=$1 AND NOT (o.objkind='routine' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2)", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name)), NULL, true);
		n = SPI_processed;
		members = palloc_array(Datum, n + 1);
		for (int i = 0; i < n; i++)
			members[i] = cell(i, 1);
		members[n++] = PointerGetDatum(member);
		database = canonical_database(json, members, n);
		move_ref(branch_ref("working"), working, database);
		advance_snapshot();

		/*
		 * The routine belongs to the branch head, whose view its validator
		 * and callers use; a revision view would make its tables read-only.
		 */
		result = CatalogViewLookupObject(provider_branch_view(dg_current_branch(), database),
										 ProcedureRelationId, name);
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
