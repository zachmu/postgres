/*
 * Rebuild a versioned trigger's pg_trigger row from its portable CREATE
 * TRIGGER statement, as CreateTrigger would, binding names in |view|.
 */
static void
provider_trigger(DGProviderRows *rows, CatalogView *view, const char *definition,
				 int count, char **definitions, char **kinds)
{
	int			slot = provider_catalog_index(TriggerRelationId);
	char	   *table = json_value(definition, "table");
	char	   *name = json_value(definition, "name");
	char	   *enabled = json_value(definition, "enabled");
	List	   *parsed = schema_expression_parse(json_value(definition, "definition"));
	CreateTrigStmt *stmt;
	FormData_pg_trigger trigger = {0};
	TupleDesc	desc = NULL;
	Node	   *when = NULL;
	int16		columns[MaxHeapAttributeNumber];
	int			ncolumns = 0;
	StringInfoData args;
	bytea	   *argbytes;
	HeapTuple	tuple;
	CatalogView *previous;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CreateTrigStmt))
		elog(ERROR, "invalid canonical trigger definition");
	stmt = castNode(CreateTrigStmt, linitial_node(RawStmt, parsed)->stmt);
	{
		/* A trigger is on a table, or INSTEAD OF on a view. */
		int			member = provider_member(table, "table");

		if (member < 0)
			member = provider_member(table, "view");
		if (member < 0)
			elog(ERROR, "canonical trigger's table is missing: %s", table);
		desc = provider_columns(definitions[member]);
	}
	trigger.oid = CatalogViewBindObject(view, TriggerRelationId, name, InvalidOid);
	trigger.tgrelid = CatalogViewLookupObject(view, RelationRelationId, table);
	namestrcpy(&trigger.tgname, stmt->trigname);

	/* The trigger function is a versioned routine or a built-in one. */
	previous = CatalogViewSwitch(view);
	PG_TRY();
	{
		trigger.tgfoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	TRIGGER_CLEAR_TYPE(trigger.tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(trigger.tgtype);
	trigger.tgtype |= stmt->timing;
	trigger.tgtype |= stmt->events;
	trigger.tgenabled = enabled[0];
	if (stmt->isconstraint)
	{
		/* As CreateTrigger: the trigger's constraint, named after it. */
		int			cslot = provider_catalog_index(ConstraintRelationId);
		FormData_pg_constraint con = {0};
		HeapTuple	contuple;

		con.oid = CatalogViewBindObject(view, ConstraintRelationId, psprintf("%s/%s", table, stmt->trigname), InvalidOid);
		namestrcpy(&con.conname, stmt->trigname);
		con.connamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(stringToQualifiedNameList(table, NULL))));
		con.contype = CONSTRAINT_TRIGGER;
		con.condeferrable = stmt->deferrable;
		con.condeferred = stmt->initdeferred;
		con.conenforced = true;
		con.convalidated = true;
		con.conrelid = trigger.tgrelid;
		con.confupdtype = ' ';
		con.confdeltype = ' ';
		con.confmatchtype = ' ';
		con.conislocal = true;
		con.connoinherit = true;
		contuple = provider_fixed(rows->relations[cslot], &con, sizeof(con), Anum_pg_constraint_conperiod);
		rows->rows[cslot] = lappend(rows->rows[cslot], contuple);
		provider_dependency(rows, ConstraintRelationId, con.oid, 0, RelationRelationId, trigger.tgrelid, 0, DEPENDENCY_AUTO);
		provider_dependency(rows, ConstraintRelationId, con.oid, 0, TriggerRelationId, trigger.oid, 0, DEPENDENCY_INTERNAL);
		trigger.tgconstraint = con.oid;
		trigger.tgdeferrable = stmt->deferrable;
		trigger.tginitdeferred = stmt->initdeferred;
		if (stmt->constrrel)
			trigger.tgconstrrelid = CatalogViewLookupObject(view, RelationRelationId,
															dg_qualified_name(stmt->constrrel->schemaname, stmt->constrrel->relname));
	}
	foreach_ptr(String, column, stmt->columns)
		columns[ncolumns++] = provider_attribute(desc, strVal(column));
	initStringInfo(&args);
	foreach_ptr(String, arg, stmt->args)
	{
		appendStringInfoString(&args, strVal(arg));
		appendStringInfoChar(&args, '\0');
	}
	trigger.tgnargs = list_length(stmt->args);
	if (stmt->whenClause)
		when = schema_expression_bind_trigger(view, trigger.tgrelid, strVal(llast(stringToQualifiedNameList(table, NULL))),
											  desc, stmt->whenClause);
	tuple = provider_fixed(rows->relations[slot], &trigger, sizeof(trigger), Anum_pg_trigger_tgnargs);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgattr, PointerGetDatum(buildint2vector(columns, ncolumns)));
	argbytes = palloc(VARHDRSZ + args.len);
	SET_VARSIZE(argbytes, VARHDRSZ + args.len);
	memcpy(VARDATA(argbytes), args.data, args.len);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgargs, PointerGetDatum(argbytes));
	if (when)
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgqual, CStringGetTextDatum(nodeToString(when)));
	foreach_ptr(TriggerTransition, transition, stmt->transitionRels)
		tuple = provider_set(rows->relations[slot], tuple,
							 transition->isNew ? Anum_pg_trigger_tgnewtable : Anum_pg_trigger_tgoldtable,
							 DirectFunctionCall1(namein, CStringGetDatum(transition->name)));
	rows->rows[slot] = lappend(rows->rows[slot], tuple);

	/* The dependencies CreateTrigger records for a user trigger. */
	provider_dependency(rows, TriggerRelationId, trigger.oid, 0, ProcedureRelationId, trigger.tgfoid, 0, DEPENDENCY_NORMAL);
	provider_dependency(rows, TriggerRelationId, trigger.oid, 0, RelationRelationId, trigger.tgrelid, 0, DEPENDENCY_AUTO);
	if (OidIsValid(trigger.tgconstrrelid))
		provider_dependency(rows, TriggerRelationId, trigger.oid, 0, RelationRelationId, trigger.tgconstrrelid, 0, DEPENDENCY_AUTO);
	for (int i = 0; i < ncolumns; i++)
		provider_dependency(rows, TriggerRelationId, trigger.oid, 0, RelationRelationId, trigger.tgrelid, columns[i], DEPENDENCY_NORMAL);
	if (when)
	{
		for (int varno = PRS2_OLD_VARNO; varno <= PRS2_NEW_VARNO; varno++)
		{
			Bitmapset  *attributes = NULL;
			int			att = -1;

			pull_varattnos(when, varno, &attributes);
			while ((att = bms_next_member(attributes, att)) >= 0)
				provider_dependency(rows, TriggerRelationId, trigger.oid, 0, RelationRelationId, trigger.tgrelid,
									att + FirstLowInvalidHeapAttributeNumber, DEPENDENCY_NORMAL);
		}
		provider_expression_dependencies(rows, TriggerRelationId, trigger.oid, when);
	}
	foreach_ptr(HeapTupleData, klass, rows->rows[provider_catalog_index(RelationRelationId)])
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(klass);

		if (form->oid == trigger.tgrelid)
			form->relhastriggers = true;
	}
}
