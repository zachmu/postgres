/* Branch-local metadata revisions and eager tuple-layout rewrites. */
static PLTree
alter_rows(DGState *state, int oldcount, int count, int *mapping)
{
	DGRows		rows = dg_rows(&state->content);
	PLEntry    *entries = palloc_array(PLEntry, Max(rows.count, 1));
	size_t		used = 0;


	for (size_t i = 0; i < rows.count; i++)
	{
		PLValue    *fields = palloc0_array(PLValue, Max(count, 1));
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		uint8	   *packed,
				   *value,
				   *key;
		size_t		length;
		PLHash		hash;

		for (int j = 0; j < count; j++)
			if (mapping[j] < oldcount)
				dg_check(pl_tuple_get(row, mapping[j], &fields[j]));
		dg_check(pl_tuple_pack(fields, count, &packed, &length));
		hash = dg_rowhash(DG_SLICE(packed, length));
		key = palloc(PL_HASH_SIZE);
		memcpy(key, hash.bytes, PL_HASH_SIZE);
		value = palloc(length + 8);
		memcpy(value, rows.items[i].value.data, 8);
		memcpy(value + 8, packed, length);
		free(packed);
		pfree(fields);
		entries[i] = DG_ENTRY(DG_SLICE(key, PL_HASH_SIZE), DG_SLICE(value, length + 8));
	}
	qsort(entries, rows.count, sizeof(*entries), entry_compare);
	for (size_t i = 0; i < rows.count; i++)
	{
		if (used && !pl_compare(entries[used - 1].key, entries[i].key))
		{
			uint64		a = pl_read64(entries[used - 1].value.data),
						b = pl_read64(entries[i].value.data);

			if (entries[used - 1].value.len != entries[i].value.len || memcmp(entries[used - 1].value.data + 8, entries[i].value.data + 8, entries[i].value.len - 8))
				elog(ERROR, "canonical row hash collision");
			if (UINT64_MAX - a < b)
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("row occurrence count overflow")));
			pl_write64((uint8 *) entries[used - 1].value.data, a + b);
		}
		else
			entries[used++] = entries[i];
	}
	{
		PLTree		result;

		dg_check(pl_build(state->store, entries, used, &result));
		return result;
	}
}

#include "dg_native_mutation.inc"

/* Replace one descriptor, preserving every unrelated canonical member. */
static bytea *
alter_database(Capture current, const char *name, bytea *member)
{
	Datum	   *children;
	int			n;
	char	   *json;

	run("SELECT jsonb_set($1::jsonb,'{members}',(SELECT jsonb_agg(CASE WHEN v->>'name'=$2 THEN $3::jsonb ELSE v END) FROM jsonb_array_elements($1::jsonb->'members') v))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(name), CStringGetTextDatum(metadata(member, "table"))), NULL, true);
	json = strcell(0, 1);
	run("SELECT CASE WHEN o.objkind='table' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2 THEN $3 ELSE ch END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1", 3, VC_TYPES(BYTEAOID, TEXTOID, BYTEAOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name), PointerGetDatum(member)), NULL, true);
	n = SPI_processed;
	children = palloc_array(Datum, Max(n, 1));
	for (int i = 0; i < n; i++)
		children[i] = cell(i, 1);
	return canonical_database(json, children, n);
}

/* Whether |rv| names a table on a branch, rather than a native relation. */
static bool
branch_relation(const RangeVar *rv)
{
	Oid			id;

	if (rv->catalogname || rv->dolt_revision)
		return true;
	id = RangeVarGetRelid(rv, NoLock, true);
	return OidIsValid(id) && CatalogViewObjectContext(id) != NULL;
}

bool
dg_catalog_alter(Node *statement)
{
	RangeVar   *rv;
	AlterTableCmd *cmd = NULL;
	CommentStmt *comment = NULL;
	Oid			user,
				id;
	int			security,
				nest;
	bool		pushed = false,
				missing = false;
	char	   *column = NULL,
			   *name;
	Relation	definition;

	if (rangevar_projection_hook || catalog_api_depth ||
		(!IsA(statement, IndexStmt) && !IsA(statement, AlterTableStmt) && !IsA(statement, CommentStmt)) ||
		!dg_catalog_active())
		return false;
	if (IsA(statement, IndexStmt))
	{
		IndexStmt  *index = copyObject((IndexStmt *) statement);
		AlterTableStmt *alter = makeNode(AlterTableStmt);
		AlterTableCmd *action = makeNode(AlterTableCmd);

		alter->relation = index->relation;
		alter->objtype = OBJECT_TABLE;
		action->subtype = AT_AddIndex;
		action->def = (Node *) index;
		alter->cmds = list_make1(action);
		statement = (Node *) alter;
	}
	if (!IsA(statement, AlterTableStmt) && !IsA(statement, CommentStmt))
		return false;
	if (IsA(statement, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *) statement;


		if (stmt->objtype != OBJECT_TABLE || !branch_relation(stmt->relation))
			return false;
		cmd = linitial_node(AlterTableCmd, stmt->cmds);
		if (native_alter_action(cmd->subtype))
		{
			ListCell   *lc;

			foreach(lc, stmt->cmds)
			{
				AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

				if (!native_alter_action(action->subtype))
					unsupported("native ALTER action lacks a portable catalog codec");

			}
		}
		else if (list_length(stmt->cmds) != 1)
			unsupported("canonical layout ALTER requires one action");
		if (!native_alter_action(cmd->subtype) && cmd->subtype != AT_AddColumn && cmd->subtype != AT_DropColumn)
			return false;

		if (cmd->behavior != DROP_RESTRICT && cmd->subtype != AT_DropColumn)
			unsupported("canonical column DROP CASCADE");
		rv = stmt->relation;
		missing = stmt->missing_ok;
	}
	else
	{
		List	   *parts;

		comment = copyObject((CommentStmt *) statement);
		if (comment->comment && !comment->comment[0])
			comment->comment = NULL;
		if (comment->objtype != OBJECT_TABLE && comment->objtype != OBJECT_COLUMN)
			return false;
		parts = copyObject((List *) comment->object);
		if (comment->objtype == OBJECT_COLUMN)
		{
			column = pstrdup(strVal(llast(parts)));
			parts = list_truncate(parts, list_length(parts) - 1);
		}
		rv = makeRangeVarFromNameList(parts);
		if (!branch_relation(rv))
			return false;
	}
	if (rv->catalogname || rv->dolt_revision)
		unsupported("revision-qualified canonical DDL");
	if (XactReadOnly)
		PreventCommandIfReadOnly("ALTER TABLE");
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	/* Lock the branch's relation as PostgreSQL locks the native one. */
	id = RangeVarGetRelid(rv, comment ? ShareUpdateExclusiveLock :
						  AlterTableGetLockLevel(((AlterTableStmt *) statement)->cmds), missing);
	if (!OidIsValid(id))
	{
		if (pushed)
			PopActiveSnapshot();
		return true;
	}
	if (!CatalogViewObjectContext(id))
		unsupported("canonical ALTER of non-versioned relation");
	definition = relation_open(id, NoLock);
	if (definition->rd_rel->relkind != RELKIND_RELATION)
		unsupported("canonical ALTER of non-table");
	GetUserIdAndSecContext(&user, &security);
	if (!object_ownercheck(RelationRelationId, id, user))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, rv->relname);
	name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(definition)), RelationGetRelationName(definition));
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		VersionRef	working;
		Capture		current;
		Relation	source;
		DGState    *state;
		bytea	   *descriptor,
				   *schema,
				   *content,
				   *member,
				   *database;
		char	   *json,
				   *payload;
		Oid			resource;
		int			att = -1,
					oldcount = RelationGetDescr(definition)->natts;

		nest = begin_api();
		run("SELECT 1 FROM pg_event_trigger WHERE evtenabled<>'D'", 0, NULL, NULL, NULL, true);
		if (SPI_processed)
			unsupported("canonical ALTER with event triggers");
		source = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
		if (RelationGetRelid(source) != id)
			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("canonical ALTER target changed; retry statement")));
		resource = dg_storage_bound(source) ? source->rd_lockInfo.lockRelId.relId : dg_identity_storage(id);
		state = dg_load(source, SnapshotSelf);
		if (column)
		{
			for (int i = 0; i < oldcount; i++)
				if (!strcmp(column, NameStr(TupleDescAttr(RelationGetDescr(source), i)->attname)))
					att = i;
			if (att < 0)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", column)));
		}
		dg_storage_close_relation(source);
		working = read_ref(branch_ref("working"));
		current = capture(true);
		if (cmd && native_alter_action(cmd->subtype))
		{
			native_alter_publish(current, working, name, (AlterTableStmt *) statement, state, oldcount, user);
			dg_release(state);
			end_api(nest);
		}
		else
		{
			run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name)), NULL, true);
			if (SPI_processed != 1)
				elog(ERROR, "canonical ALTER member missing");
			descriptor = DatumGetByteaPCopy(cell(0, 1));
			json = metadata(child(descriptor, 1), "schema");
			{
				char	   *path = column ? psprintf("{columns,%d,comment}", att) : "{comment}";

				run("SELECT jsonb_set($1::jsonb,$2::text[],coalesce(to_jsonb($3::text),'null'::jsonb))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(path), comment->comment ? CStringGetTextDatum(comment->comment) : (Datum) 0), comment->comment ? NULL : "  n", true);
				json = strcell(0, 1);
			}
			schema = put("schema", json, NULL, 0);
			content = child(descriptor, 2);
			payload = metadata(descriptor, "table");
			run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payload), CStringGetTextDatum(json)), NULL, true);
			member = put("table", strcell(0, 1), VC_ARGS(PointerGetDatum(schema), PointerGetDatum(content)), 2);
			database = alter_database(current, name, member);
			binding_record(dg_current_branch(), name, schema, resource);
			move_ref(branch_ref("working"), working, database);
			advance_snapshot();
			dg_release(state);
			end_api(nest);
		}
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		relation_close(definition, NoLock);
		if (pushed)
			PopActiveSnapshot();
	} PG_END_TRY();
	return true;
}

/* Publish table |name|'s definition |json| on the session's branch. */
static void
publish_table_definition(Capture current, VersionRef working, const char *name, const char *json)
{
	bytea	   *descriptor,
			   *schema,
			   *member;

	run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name)), NULL, true);
	if (SPI_processed != 1)
		elog(ERROR, "canonical table member %s missing", name);
	descriptor = DatumGetByteaPCopy(cell(0, 1));
	schema = put("schema", json, NULL, 0);
	run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(metadata(descriptor, "table")), CStringGetTextDatum(json)), NULL, true);
	member = put("table", strcell(0, 1), VC_ARGS(PointerGetDatum(schema), PointerGetDatum(child(descriptor, 2))), 2);
	binding_record(dg_current_branch(), name, schema, binding_relation(dg_current_branch(), name, child(descriptor, 1)));
	move_ref(branch_ref("working"), working, alter_database(current, name, member));
	advance_snapshot();
}

/*
 * GRANT and REVOKE on tables of the session's branch. The native command runs
 * against a private copy of the branch's catalogs, so it applies PostgreSQL's
 * own privilege rules; the resulting ACL is then published in each table's
 * definition.
 */
bool
dg_catalog_grant(Node *statement)
{
	GrantStmt  *grant;
	List	   *names = NIL;
	ListCell   *lc;
	int			native = 0;
	Oid			user;
	int			security,
				nest;

	if (!IsA(statement, GrantStmt) || rangevar_projection_hook || catalog_api_depth)
		return false;
	grant = (GrantStmt *) statement;
	if (grant->targtype != ACL_TARGET_OBJECT || grant->objtype != OBJECT_TABLE || !dg_catalog_active())
		return false;
	foreach(lc, grant->objects)
	{
		RangeVar   *rv = lfirst_node(RangeVar, lc);
		Oid			id;

		if (!branch_relation(rv))
		{
			native++;
			continue;
		}
		if (rv->catalogname || rv->dolt_revision)
			unsupported("revision-qualified GRANT");
		id = RangeVarGetRelid(rv, NoLock, false);
		names = lappend(names, quote_qualified_identifier(get_namespace_name(get_rel_namespace(id)), get_rel_name(id)));
	}
	if (!names)
		return false;
	if (native)
		unsupported("GRANT on versioned and unversioned tables together");
	foreach(lc, grant->privileges)
		if (lfirst_node(AccessPriv, lc)->cols)
			unsupported("column privileges on versioned tables");
	PreventCommandIfReadOnly(grant->is_grant ? "GRANT" : "REVOKE");
	PushActiveSnapshot(GetLatestSnapshot());
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		Capture		current;
		CatalogView *view = CatalogViewCreate("provisional GRANT");
		CatalogView *previous = CatalogViewCurrent();
		List	   *acls = NIL;
		ListCell   *ac;

		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		CatalogViewBeginMutation(view);
		PG_TRY(provisional);
		{
			int			pathnest = NewGUCNestLevel();

			CatalogViewSwitch(view);
			SetUserIdAndSecContext(user, security);
			set_config_option("search_path", api_search_path, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
			ExecuteGrantStmt(copyObject(grant));
			CommandCounterIncrement();
			AtEOXact_GUC(true, pathnest);
			SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
			foreach(lc, names)
			{
				Oid			handle = CatalogViewLookupObject(view, RelationRelationId, lfirst(lc));
				HeapTuple	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(handle));
				bool		isnull;
				Datum		acl;

				if (!HeapTupleIsValid(tuple))
					elog(ERROR, "provisional relation %u missing", handle);
				acl = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl, &isnull);
				acls = lappend(acls, isnull ? NULL : DatumGetAclPCopy(acl));
			}
		}
		PG_FINALLY(provisional);
		{
			SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
			CatalogViewSwitch(previous);
			CatalogViewRetireMutation(view);
			CatalogViewRelease(view);
		}
		PG_END_TRY(provisional);
		forboth(lc, names, ac, acls)
		{
			const char *name = lfirst(lc);
			Acl		   *acl = lfirst(ac);
			char	   *definition = NULL;

			run("SELECT t.objchildren[1] FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(read_ref(branch_ref("working")).target), CStringGetTextDatum(name)), NULL, true);
			definition = metadata(DatumGetByteaPCopy(cell(0, 1)), "schema");
			run("SELECT jsonb_set($1::jsonb,'{acl}',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode(COALESCE($2::aclitem[],acldefault('r',(SELECT oid FROM pg_authid WHERE rolname=$1::jsonb->>'owner')))) x),'[]'::jsonb))::text", 2, VC_TYPES(TEXTOID, ACLITEMARRAYOID), VC_ARGS(CStringGetTextDatum(definition), PointerGetDatum(acl)), acl ? NULL : " n", true);
			definition = strcell(0, 1);
			publish_table_definition(capture(true), read_ref(branch_ref("working")), name, definition);
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}
