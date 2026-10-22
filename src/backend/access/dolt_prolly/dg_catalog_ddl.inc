/* CREATE scope only admits the newly allocated table and its own indexes. */
static bool
catalog_new_member(Oid id)
{
	if (!OidIsValid(catalog_new_relation))
		return false;
	return id == catalog_new_relation ||
		(get_rel_relkind(id) == RELKIND_INDEX && IndexGetRelation(id, false) == catalog_new_relation);
}

bool
dg_catalog_creating_relation(Relation rel)
{
	return catalog_new_member(RelationGetRelid(rel));
}

void
dg_catalog_create_scope(bool enter)
{
	Assert(!enter || !catalog_creating);
	catalog_creating = enter;
	catalog_new_relation = InvalidOid;
}

void
dg_catalog_created(Oid id)
{
	if (catalog_creating)
	{
		Assert(!OidIsValid(catalog_new_relation));
		catalog_new_relation = id;
	}
}

/* Only the native index subcommands emitted by this CREATE may pass the guard. */
bool
dg_catalog_create_index(Node *statement)
{
	IndexStmt  *stmt;

	if (!catalog_creating || !OidIsValid(catalog_new_relation) || !IsA(statement, IndexStmt))
		return false;
	stmt = (IndexStmt *) statement;
	return stmt->isconstraint && !stmt->concurrent &&
		RangeVarGetRelid(stmt->relation, NoLock, false) == catalog_new_relation;
}

/* First writable provider operation: remove branch membership, not storage.
 * The physical catalog remains a runtime bridge for surviving definitions. */
void
dg_catalog_check_relation(Relation rel, const char *branch, Snapshot snapshot)
{
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;

	if (!IsUnderPostmaster || catalog_new_member(RelationGetRelid(rel)))
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
		if (db)
		{
			Oid			bound;
			bytea	   *schema;

			run("SELECT t.objchildren[1] FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index') AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
			schema = DatumGetByteaPCopy(cell(0, 1));
			bound = binding_relation(branch, name, schema);
			if (bound != RelationGetRelid(rel))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("runtime catalog binding mismatch for %s on branch %s", name, branch)));
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
			dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
			working = read_ref(branch_ref("working"));
			foreach(lc, drop->objects)
			{
				RangeVar   *rv = makeRangeVarFromNameList(lfirst(lc));
				Oid			id;
				Relation	rel;
				char	   *name;
				ListCell   *index;

				List	   *path = NIL;
				ListCell   *part;
				CatalogView *view = provider_view(working.target);

				id = InvalidOid;
				if (rv->catalogname || rv->dolt_revision)
					unsupported("qualified revision DROP");
				if (rv->schemaname)
					path = list_make1(rv->schemaname);
				else if (!SplitIdentifierString(pstrdup(search_path), ',', &path))
					elog(ERROR, "invalid search_path");
				foreach(part, path)
				{
					char	   *ns = (char *) lfirst(part);

					if (!strcmp(ns, "$user"))
						ns = GetUserNameFromId(user, false);
					name = quote_qualified_identifier(ns, rv->relname);
					if (catalog_contains(working.target, name))
					{
						catalog_acl(catalog_namespace(working.target, ns), "USAGE", user);
						id = CatalogViewLookupObject(view, RelationRelationId, name);
						break;
					}
				}
				if (!OidIsValid(id) && !drop->missing_ok)
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s does not exist on branch %s", rv->relname, dg_current_branch())));
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
				run("SELECT ch FROM pg_dolt_object_visible db CROSS JOIN LATERAL unnest(db.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE db.objhash=$1 AND CASE WHEN o.objkind IN ('table','index') THEN NOT (convert_from(o.objpayload,'UTF8')::jsonb->>'name'=ANY($2)) ELSE true END ORDER BY ch", 2, VC_TYPES(BYTEAOID, TEXTARRAYOID), VC_ARGS(PointerGetDatum(current.hash), PointerGetDatum(array)), NULL, true);
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

/* Validate and serialize before running ordinary CREATE as the caller. */
bool
dg_catalog_create_begin(Node *statement)
{
	CreateStmt *stmt;
	Oid			user;
	int			security,
				nest;
	bool		active = false;
	bool		pushed = false;
	ListCell   *lc;

	if (!IsUnderPostmaster || !IsA(statement, CreateStmt))
		return false;
	stmt = (CreateStmt *) statement;
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
		active = read_ref("views/enabled").target != NULL;
		if (active)
		{
			lock_workspace();
			if (IsolationUsesXactSnapshot())
				unsupported("branch CREATE under Repeatable Read or Serializable");
			run("SELECT 1 FROM pg_event_trigger WHERE evtenabled <> 'D'", 0, NULL, NULL, NULL, true);
			if (SPI_processed)
				unsupported("branch CREATE with event triggers");
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
	if (!active)
		return false;
	if (stmt->relation->relpersistence != RELPERSISTENCE_PERMANENT ||
		stmt->inhRelations || stmt->partbound || stmt->partspec ||
		stmt->ofTypename || stmt->options || stmt->tablespacename ||
		(stmt->accessMethod && strcmp(stmt->accessMethod, "dolt_prolly_table")))
		unsupported("branch CREATE requires a plain permanent prolly table");
	foreach(lc, stmt->tableElts)
	{
		ColumnDef  *col = lfirst(lc);
		ListCell   *cc;

		if (IsA(col, Constraint))
		{
			Constraint *con = (Constraint *) col;

			if (con->deferrable || (con->contype != CONSTR_PRIMARY && con->contype != CONSTR_UNIQUE))
				unsupported("branch CREATE table constraint");
			continue;
		}
		if (!IsA(col, ColumnDef))
			unsupported("branch CREATE LIKE");
		foreach(cc, col->constraints)
		{
			Constraint *con = lfirst(cc);

			if (con->deferrable || (con->contype != CONSTR_NULL && con->contype != CONSTR_NOTNULL &&
									con->contype != CONSTR_PRIMARY && con->contype != CONSTR_UNIQUE))
				unsupported("branch CREATE supports only NOT NULL, PRIMARY KEY and UNIQUE constraints");
		}
	}
	/* IF NOT EXISTS must not silently accept a retained object absent here. */
	{
		Oid			existing;
		Oid			namespaceid = RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, &existing);

		stmt->relation->schemaname = get_namespace_name(namespaceid);

		if (OidIsValid(existing))
		{
			Relation	rel = table_open(existing, AccessShareLock);

			dg_catalog_check_relation(rel, dg_current_branch(), GetTransactionSnapshot());
			table_close(rel, AccessShareLock);
		}
	}
	stmt->accessMethod = pstrdup("dolt_prolly_table");
	return true;
}

/* The relation has been created as the caller; capture and ref publication are
 * privileged, fixed-query operations in that very same SQL transaction. */
void
dg_catalog_create_finish(CreateStmt *stmt)
{
	Oid			id = RangeVarGetRelid(stmt->relation, AccessExclusiveLock, false);
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;

	GetUserIdAndSecContext(&user, &security);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		VersionRef	working;
		Capture		current;
		Relation	rel;
		DGState    *state;

		nest = begin_api();
		working = read_ref(branch_ref("working"));
		rel = table_open(id, NoLock);
		if (catalog_contains(working.target, quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel))))
		{
			/* Existing member with IF NOT EXISTS: do not reset its content. */
			table_close(rel, NoLock);
		}
		else
		{
			catalog_new_relation = id;
			/* A new table has no HEAD entry, even on a non-main branch. */
			state = dg_load_branch(rel, GetActiveSnapshot(), dg_current_branch());
			dg_save(rel, state, GetCurrentCommandId(true));
			dg_release(state);
			table_close(rel, NoLock);
			CommandCounterIncrement();
			current = capture(true);
			move_ref(branch_ref("working"), working, current.hash);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		catalog_new_relation = InvalidOid;
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
}
