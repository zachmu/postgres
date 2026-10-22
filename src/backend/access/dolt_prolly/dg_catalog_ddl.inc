/* First writable provider operation: remove branch membership, not storage.
 * The physical catalog remains a runtime bridge for surviving definitions. */
void
dg_catalog_check_relation(Relation rel, const char *branch, Snapshot snapshot)
{
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;

	if (!IsUnderPostmaster)
		return;
	GetUserIdAndSecContext(&user, &security);
	if (!ActiveSnapshotSet() || (snapshot && IsMVCCSnapshot(snapshot)))
	{
		PushActiveSnapshot(snapshot && IsMVCCSnapshot(snapshot) ? snapshot : GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *db;
		char	   *name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));

		nest = begin_api();
		db = catalog_database(branch);
		if (!catalog_contains(db, name))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", name, branch)));
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
}

bool
dg_catalog_drop(Node *statement)
{
	DropStmt   *drop;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	bool		handled = false;
	char	   *search_path = pstrdup(GetConfigOption("search_path", false, false));

	if (!IsUnderPostmaster || !IsA(statement, DropStmt))
		return false;
	drop = (DropStmt *) statement;
	if (drop->removeType != OBJECT_TABLE)
		return false;
	GetUserIdAndSecContext(&user, &security);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		LockDatabaseObject(DoltRefRelationId, 0, 0, ExclusiveLock);
		if (read_ref("views/enabled").target)
		{
			ListCell   *lc;
			List	   *removed = NIL;
			VersionRef	working;
			Capture		current;
			Datum	   *names;
			int			i = 0;
			bytea	   *database;
			char	   *json;
			Datum	   *children;
			int			nchildren;

			handled = true;
			if (drop->behavior != DROP_RESTRICT || drop->concurrent)
				unsupported("branch DROP supports RESTRICT only");
			lock_workspace();
			working = read_ref(branch_ref("working"));
			foreach(lc, drop->objects)
			{
				RangeVar   *rv = makeRangeVarFromNameList(lfirst(lc));
				Oid			id;
				Relation	rel;
				char	   *name;
				ListCell   *index;

				SetUserIdAndSecContext(user, security);
				set_config_option("search_path", search_path, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
				id = RangeVarGetRelid(rv, AccessExclusiveLock, drop->missing_ok);
				SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
				set_config_option("search_path", "pg_catalog, pg_temp", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
				if (!OidIsValid(id))
					continue;
				rel = table_open(id, NoLock);
				name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));
				if (!catalog_contains(working.target, name))
				{
					table_close(rel, NoLock);
					if (drop->missing_ok)
						continue;
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", name, dg_current_branch())));
				}
				if (rel->rd_tableam != dg_table_routine())
					unsupported("branch DROP of non-prolly tables");
				if (!object_ownercheck(RelationRelationId, id, user) && !object_ownercheck(NamespaceRelationId, RelationGetNamespace(rel), user))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, RelationGetRelationName(rel));
				removed = lappend(removed, name);
				foreach(index, RelationGetIndexList(rel))
				{
					Oid			indexid = lfirst_oid(index);

					removed = lappend(removed, quote_qualified_identifier(get_namespace_name(get_rel_namespace(indexid)), get_rel_name(indexid)));
				}
				table_close(rel, NoLock);
			}
			current = capture(true);
			names = palloc(sizeof(Datum) * Max(list_length(removed), 1));
			foreach(lc, removed)
				names[i++] = CStringGetTextDatum((char *) lfirst(lc));
			{
				ArrayType  *array = i ? construct_array(names, i, TEXTOID, -1, false, TYPALIGN_INT) : construct_empty_array(TEXTOID);

				run("SELECT jsonb_set($1::jsonb,'{members}',coalesce((SELECT jsonb_agg(v ORDER BY v->>'kind' COLLATE \"C\",coalesce(v->>'name',v->'definition'->>'name') COLLATE \"C\") FROM jsonb_array_elements($1::jsonb->'members') v WHERE NOT (coalesce(v->>'name','')=ANY($2))),'[]'::jsonb))::text", 2, VC_TYPES(TEXTOID, TEXTARRAYOID), VC_ARGS(CStringGetTextDatum(current.json), PointerGetDatum(array)), NULL, true);
				json = strcell(0, 1);
				run("SELECT ch FROM pg_dolt_object db CROSS JOIN LATERAL unnest(db.objchildren) ch JOIN pg_dolt_object o ON o.objhash=ch WHERE db.objhash=$1 AND CASE WHEN o.objkind IN ('table','index') THEN NOT (convert_from(o.objpayload,'UTF8')::jsonb->>'name'=ANY($2)) ELSE true END ORDER BY ch", 2, VC_TYPES(BYTEAOID, TEXTARRAYOID), VC_ARGS(PointerGetDatum(current.hash), PointerGetDatum(array)), NULL, true);
			}
			nchildren = SPI_processed;
			children = palloc(sizeof(Datum) * Max(nchildren, 1));
			for (i = 0; i < nchildren; i++)
				children[i] = cell(i, 1);
			database = put("database", json, children, nchildren);
			move_ref(branch_ref("working"), working, database);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return handled;
}
