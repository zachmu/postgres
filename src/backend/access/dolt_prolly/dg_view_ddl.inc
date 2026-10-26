/*
 * Versioned views. A view is a schema member of its own: its query as SQL with
 * every name qualified, its columns by name and portable type, its options,
 * and the objects its query depends on, by name. The provider rebuilds the
 * view's catalog rows from these; its rewrite rule holds the SQL, which is
 * analyzed when the rule is first read, in the complete catalog view of the
 * view's revision, as SQL routine bodies are.
 */

static int
view_entry_compare(const ListCell *a, const ListCell *b)
{
	return strcmp(strVal(lfirst(a)), strVal(lfirst(b)));
}

/* The rewrite rule of view |relid|, in the current catalog view. */
static HeapTuple
view_rule(Oid relid)
{
	Relation	catalog = table_open(RewriteRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tuple,
				result = NULL;

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		if (((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class == relid &&
			!strcmp(NameStr(((Form_pg_rewrite) GETSTRUCT(tuple))->rulename), ViewSelectRuleName))
			result = heap_copytuple(tuple);
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (!result)
		elog(ERROR, "view %u has no rule", relid);
	return result;
}

/* A dependency's object by portable name, or NULL for a built-in one. */
static char *
view_dependency_name(Oid classid, Oid objid)
{
	if (objid < FirstNormalObjectId)
		return NULL;
	if (classid == RelationRelationId)
		return dg_qualified_name(get_namespace_name(get_rel_namespace(objid)), get_rel_name(objid));
	if (classid == ProcedureRelationId)
	{
		if (objid < FirstDurableObjectId)
			unsupported("versioned view calling an unversioned function");
		return pstrdup(CatalogViewObjectIdentity(objid, ProcedureRelationId));
	}
	if (classid == TypeRelationId)
		return routine_type_name(objid);
	unsupported("versioned view depending on this kind of object");
	pg_unreachable();
}

static char *
view_encode(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	HeapTuple	rule = view_rule(relid);
	Oid			ruleid = ((Form_pg_rewrite) GETSTRUCT(rule))->oid;
	StringInfoData json;
	char	   *sql = NULL;
	int			nest = NewGUCNestLevel();
	List	   *dependencies = NIL;

	PG_TRY();
	{
		/* As pg_get_viewdef, from the rule itself: SQL cannot read a provisional view. */
		bool		isnull;
		Relation	rules = table_open(RewriteRelationId, AccessShareLock);
		List	   *actions = (List *) stringToNode(TextDatumGetCString(heap_getattr(rule, Anum_pg_rewrite_ev_action,
																						RelationGetDescr(rules), &isnull)));

		table_close(rules, AccessShareLock);
		actions = dg_rule_actions(relid, actions);
		schema_expression_gucs();
		sql = pg_get_querydef(linitial_node(Query, actions), false);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGVIEW1\",");
	json_field(&json, "name", dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel)), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "owner", GetUserNameFromId(rel->rd_rel->relowner, false), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "query", sql, false);
	appendStringInfoString(&json, ",\"options\":[");
	{
		bool		isnull;
		HeapTuple	klass = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		Datum		options = SysCacheGetAttr(RELOID, klass, Anum_pg_class_reloptions, &isnull);

		if (!isnull)
		{
			Datum	   *items;
			int			n;

			deconstruct_array_builtin(DatumGetArrayTypeP(options), TEXTOID, &items, NULL, &n);
			for (int i = 0; i < n; i++)
			{
				if (i)
					appendStringInfoChar(&json, ',');
				escape_json(&json, TextDatumGetCString(items[i]));
			}
		}
		ReleaseSysCache(klass);
	}
	appendStringInfoString(&json, "],\"columns\":[");
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (i)
			appendStringInfoChar(&json, ',');
		appendStringInfoChar(&json, '{');
		json_field(&json, "name", NameStr(attr->attname), false);
		appendStringInfoChar(&json, ',');
		json_field(&json, "type", routine_type_name(attr->atttypid), false);
		appendStringInfo(&json, ",\"typmod\":%d", attr->atttypmod);
		if (attr->attcollation != get_typcollation(attr->atttypid))
		{
			appendStringInfoChar(&json, ',');
			json_field(&json, "collation", type_collation_name(attr->attcollation), false);
		}
		appendStringInfoChar(&json, '}');
	}
	appendStringInfoString(&json, "],\"dependencies\":[");

	/* As the rule's native dependencies, by name: [kind, name, column]. */
	{
		Relation	catalog = table_open(DependRelationId, AccessShareLock);
		SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
		HeapTuple	tuple;

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tuple);
			char	   *name;
			StringInfoData entry;

			if (dep->classid != RewriteRelationId || dep->objid != ruleid || dep->refobjid == relid ||
				!(name = view_dependency_name(dep->refclassid, dep->refobjid)))
				continue;
			initStringInfo(&entry);
			appendStringInfoChar(&entry, '[');
			escape_json(&entry, dep->refclassid == RelationRelationId ? "table" : dep->refclassid == ProcedureRelationId ? "routine" : "type");
			appendStringInfoChar(&entry, ',');
			escape_json(&entry, name);
			appendStringInfoChar(&entry, ',');
			if (dep->refclassid == RelationRelationId && dep->refobjsubid > 0)
				escape_json(&entry, get_attname(dep->refobjid, dep->refobjsubid, false));
			else
				appendStringInfoString(&entry, "null");
			appendStringInfoChar(&entry, ']');
			if (!list_member(dependencies, makeString(entry.data)))
				dependencies = lappend(dependencies, makeString(entry.data));
		}
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
	}
	list_sort(dependencies, view_entry_compare);
	foreach_node(String, entry, dependencies)
		appendStringInfo(&json, "%s%s", foreach_current_index(entry) ? "," : "", strVal(entry));
	appendStringInfoString(&json, "]}");
	relation_close(rel, AccessShareLock);
	return json.data;
}

/*
 * Analyze a versioned view's rule, which holds the view's SQL, in its
 * relation's catalog view. Other rules are returned as they are.
 */
List *
dg_rule_actions(Oid relid, List *actions)
{
	char	   *sql;
	List	   *parsed;
	Query	   *query;
	int			nest;
	CatalogView *previous;

	if (list_length(actions) != 1 || !IsA(linitial(actions), String))
		return actions;
	sql = strVal(linitial(actions));
	nest = NewGUCNestLevel();
	previous = CatalogViewSwitch(CatalogViewObjectContext(relid) ? CatalogViewObjectContext(relid) : CatalogViewCurrent());
	PG_TRY();
	{
		schema_expression_gucs();
		parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
		if (list_length(parsed) != 1)
			elog(ERROR, "invalid versioned view query");
		query = parse_analyze_fixedparams(linitial_node(RawStmt, parsed), sql, NULL, 0, NULL);
		AcquireRewriteLocks(query, false, false);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return list_make1(query);
}

/*
 * Store the analyzed rules of |views|, (kind, name) pairs, in mutable
 * provisional view |view|, so that native DDL that renames what they refer to
 * changes their queries as it changes any other definition.
 */
static void
view_materialize_rules(CatalogView *view, List *views)
{
	CatalogView *previous = CatalogViewSwitch(view);
	Relation	catalog = table_open(RewriteRelationId, RowExclusiveLock);

	foreach_ptr(List, target, views)
	{
		Oid			relid;
		HeapTuple	rule,
					updated;
		bool		isnull;
		Datum		values[Natts_pg_rewrite] = {0};
		bool		nulls[Natts_pg_rewrite] = {0};
		bool		replace[Natts_pg_rewrite] = {0};
		List	   *actions;

		if (strcmp(linitial(target), "view"))
			continue;
		relid = CatalogViewLookupObject(view, RelationRelationId, lsecond(target));
		rule = view_rule(relid);
		actions = (List *) stringToNode(TextDatumGetCString(heap_getattr(rule, Anum_pg_rewrite_ev_action, RelationGetDescr(catalog), &isnull)));
		values[Anum_pg_rewrite_ev_action - 1] = CStringGetTextDatum(nodeToString(dg_rule_actions(relid, actions)));
		replace[Anum_pg_rewrite_ev_action - 1] = true;
		updated = heap_modify_tuple(rule, RelationGetDescr(catalog), values, nulls, replace);
		CatalogTupleUpdate(catalog, &rule->t_self, updated);
	}
	table_close(catalog, RowExclusiveLock);
	CommandCounterIncrement();
	CatalogViewSwitch(previous);
}

/* Whether CREATE VIEW makes a versioned view: a permanent view in a user schema. */
static bool
creates_versioned_view(ViewStmt *stmt)
{
	Oid			namespace;

	if (stmt->view->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	namespace = RangeVarGetCreationNamespace(stmt->view);
	return !isAnyTempNamespace(namespace) && !dg_system_namespace(get_namespace_name(namespace));
}

static void
view_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_rel->relkind != RELKIND_VIEW)
		unsupported("storage effects of view DDL");
}

static void
view_create(ParseState *pstate, ViewStmt *stmt, int location, int length)
{
	CatalogView *view = CatalogViewCreate("provisional view DDL");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	Oid			caller,
				privileged;
	int			security,
				nest;

	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture		current;
		Oid			relid;
		char	   *definition;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, view_storage);
		native_mutation_view = view;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		{
			/* CREATE OR REPLACE reads the old rule. */
			Oid			existing = RangeVarGetRelid(stmt->view, NoLock, true);

			if (OidIsValid(existing) && get_rel_relkind(existing) == RELKIND_VIEW)
				view_materialize_rules(view, list_make1(list_make2("view", CatalogViewObjectIdentity(existing, RelationRelationId))));
		}
		relid = DefineView(stmt, pstate->p_sourcetext, location, length).objectId;
		CommandCounterIncrement();
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		definition = view_encode(relid);
		CatalogViewSwitch(NULL);
		nest = begin_api();
		(void) objects_publish("view", list_make1(json_value(definition, "name")), NULL, false, list_make1(definition));
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		native_mutation_view = saved_mutation;
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}

bool
dg_catalog_view_ddl(ParseState *pstate, Node *statement, int location, int length)
{
	bool		pushed = false;

	if (!IsA(statement, ViewStmt) || !dg_catalog_active() || !creates_versioned_view((ViewStmt *) statement))
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("view DDL in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	view_create(pstate, (ViewStmt *) statement, location, length);
	if (pushed)
		PopActiveSnapshot();
	return true;
}
