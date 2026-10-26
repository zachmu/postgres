/*
 * Versioned triggers. A trigger is a schema member of its own, apart from its
 * table, so that adding one does not change the table's definition. Its
 * portable form is the CREATE TRIGGER statement PostgreSQL deparses for it,
 * with every name qualified, plus its enabled state.
 */
static bool
trigger_native_function(Node *node, void *context)
{
	if (!node)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid >= FirstNormalObjectId &&
		((FuncExpr *) node)->funcid < FirstDurableObjectId)
		return true;
	if (IsA(node, OpExpr) && ((OpExpr *) node)->opno >= FirstNormalObjectId)
		return true;
	return expression_tree_walker(node, trigger_native_function, context);
}

static char *
trigger_encode(HeapTuple tuple, TupleDesc desc)
{
	Form_pg_trigger trigger = (Form_pg_trigger) GETSTRUCT(tuple);
	char	   *table = dg_qualified_name(get_namespace_name(get_rel_namespace(trigger->tgrelid)),
										  get_rel_name(trigger->tgrelid));
	char	   *sql = NULL;
	bool		isnull;
	Datum		qual = heap_getattr(tuple, Anum_pg_trigger_tgqual, desc, &isnull);
	int			nest = NewGUCNestLevel();
	StringInfoData json;
	char		enabled[2] = {trigger->tgenabled, '\0'};

	if (trigger->tgisinternal || OidIsValid(trigger->tgconstraint))
		unsupported("versioned constraint triggers");
	if (trigger->tgfoid >= FirstNormalObjectId && trigger->tgfoid < FirstDurableObjectId)
		unsupported("versioned trigger calling an unversioned function");
	if (!isnull && trigger_native_function(stringToNode(TextDatumGetCString(qual)), NULL))
		unsupported("versioned trigger condition calling an unversioned function");
	PG_TRY();
	{
		schema_expression_gucs();
		sql = TextDatumGetCString(DirectFunctionCall1(pg_get_triggerdef, ObjectIdGetDatum(trigger->oid)));
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGTRIGGER1\",");
	json_field(&json, "name", psprintf("%s/%s", table, NameStr(trigger->tgname)), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "table", table, false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "enabled", enabled, false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "definition", sql, false);
	appendStringInfoChar(&json, '}');
	return json.data;
}

/*
 * CREATE TRIGGER on a branch table runs natively against a provisional copy of
 * the branch's catalogs. The table's triggers there are then encoded and
 * replace its trigger members.
 */
static void
trigger_create(ParseState *pstate, CreateTrigStmt *stmt)
{
	CatalogView *view = CatalogViewCreate("provisional trigger DDL");
	CatalogView *previous = CatalogViewCurrent();
	Oid			caller,
				privileged;
	int			security,
				nest;
	List	   *definitions = NIL;

	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture		current;
		Relation	catalog;
		SysScanDesc scan;
		HeapTuple	tuple;
		ScanKeyData key;
		Oid			relid;
		char	   *table;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		(void) CreateTrigger(stmt, pstate->p_sourcetext, InvalidOid, InvalidOid, InvalidOid, InvalidOid,
							 InvalidOid, InvalidOid, NULL, false, false);
		CommandCounterIncrement();
		relid = RangeVarGetRelid(stmt->relation, NoLock, false);
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		catalog = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		scan = systable_beginscan(catalog, TriggerRelidNameIndexId, true, NULL, 1, &key);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			if (!((Form_pg_trigger) GETSTRUCT(tuple))->tgisinternal)
				definitions = lappend(definitions, trigger_encode(tuple, RelationGetDescr(catalog)));
		systable_endscan(scan);
		table = dg_qualified_name(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid));
		table_close(catalog, AccessShareLock);
		CatalogViewSwitch(NULL);
		(void) objects_publish("trigger", NIL, psprintf("%s/", table), false, definitions);
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}

bool
dg_catalog_trigger_ddl(ParseState *pstate, Node *statement)
{
	CreateTrigStmt *stmt;
	bool		pushed = false;

	if (!IsA(statement, CreateTrigStmt) || !dg_catalog_active())
		return false;
	stmt = (CreateTrigStmt *) statement;
	if (!branch_relation(stmt->relation))
		return false;
	if (stmt->isconstraint)
		unsupported("versioned constraint triggers");
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("trigger DDL in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	trigger_create(pstate, stmt);
	if (pushed)
		PopActiveSnapshot();
	return true;
}
