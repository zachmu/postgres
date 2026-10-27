/*
 * Reconstruct the native constraint triggers, RI and deferred unique checks,
 * from portable constraint metadata.
 */
static Oid
foreign_action(char action, bool update)
{
	switch (action)
	{
		case FKCONSTR_ACTION_NOACTION:
			return update ? F_RI_FKEY_NOACTION_UPD : F_RI_FKEY_NOACTION_DEL;
		case FKCONSTR_ACTION_RESTRICT:
			return update ? F_RI_FKEY_RESTRICT_UPD : F_RI_FKEY_RESTRICT_DEL;
		case FKCONSTR_ACTION_CASCADE:
			return update ? F_RI_FKEY_CASCADE_UPD : F_RI_FKEY_CASCADE_DEL;
		case FKCONSTR_ACTION_SETNULL:
			return update ? F_RI_FKEY_SETNULL_UPD : F_RI_FKEY_SETNULL_DEL;
		case FKCONSTR_ACTION_SETDEFAULT:
			return update ? F_RI_FKEY_SETDEFAULT_UPD : F_RI_FKEY_SETDEFAULT_DEL;
	}
	elog(ERROR, "invalid canonical foreign key action");
	return InvalidOid;
}

static void
provider_foreign_trigger(DGProviderRows *rows, CatalogView *view,
						 Form_pg_constraint con, int which)
{
	FormData_pg_trigger trigger = {0};
	int			slot = provider_catalog_index(TriggerRelationId);
	HeapTuple	tuple;
	bytea	   *args = palloc0(VARHDRSZ);
	bool		check = which >= 2;
	bool		update = which % 2;
	char		action = update ? con->confupdtype : con->confdeltype;

	trigger.oid = CatalogViewBindObject(view, TriggerRelationId,
										psprintf("%s/%d", CatalogViewObjectIdentity(con->oid, ConstraintRelationId), which), InvalidOid);
	trigger.tgrelid = check ? con->conrelid : con->confrelid;
	trigger.tgconstrrelid = check ? con->confrelid : con->conrelid;
	trigger.tgconstraint = con->oid;
	trigger.tgconstrindid = con->conindid;
	snprintf(NameStr(trigger.tgname), NAMEDATALEN, "RI_ConstraintTrigger_%c_%u", check ? 'c' : 'a', trigger.oid);
	trigger.tgfoid = check ? (update ? F_RI_FKEY_CHECK_UPD : F_RI_FKEY_CHECK_INS) : foreign_action(action, update);
	trigger.tgtype = TRIGGER_TYPE_ROW | (update ? TRIGGER_TYPE_UPDATE : check ? TRIGGER_TYPE_INSERT : TRIGGER_TYPE_DELETE);
	trigger.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trigger.tgisinternal = true;
	trigger.tgdeferrable = (check || action == FKCONSTR_ACTION_NOACTION) && con->condeferrable;
	trigger.tginitdeferred = trigger.tgdeferrable && con->condeferred;
	tuple = provider_fixed(rows->relations[slot], &trigger, sizeof(trigger), Anum_pg_trigger_tgnargs);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgattr, PointerGetDatum(buildint2vector(NULL, 0)));
	SET_VARSIZE(args, VARHDRSZ);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgargs, PointerGetDatum(args));
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
	provider_dependency(rows, TriggerRelationId, trigger.oid, 0, ConstraintRelationId, con->oid, 0, DEPENDENCY_INTERNAL);
	foreach_ptr(HeapTupleData, klass, rows->rows[provider_catalog_index(RelationRelationId)])
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(klass);

		if (form->oid == trigger.tgrelid)
			form->relhastriggers = true;
	}
}

/*
 * As index_constraint_create, the internal trigger that rechecks a deferrable
 * primary key or unique constraint |con| after the rows it queued change.
 */
static void
provider_unique_trigger(DGProviderRows *rows, CatalogView *view, Form_pg_constraint con)
{
	FormData_pg_trigger trigger = {0};
	int			slot = provider_catalog_index(TriggerRelationId);
	HeapTuple	tuple;
	bytea	   *args = palloc0(VARHDRSZ);

	trigger.oid = CatalogViewBindObject(view, TriggerRelationId,
										psprintf("%s/recheck", CatalogViewObjectIdentity(con->oid, ConstraintRelationId)), InvalidOid);
	trigger.tgrelid = con->conrelid;
	trigger.tgconstraint = con->oid;
	trigger.tgconstrindid = con->conindid;
	snprintf(NameStr(trigger.tgname), NAMEDATALEN, "%s_%u",
			 con->contype == CONSTRAINT_PRIMARY ? "PK_ConstraintTrigger" : "Unique_ConstraintTrigger", trigger.oid);
	trigger.tgfoid = F_UNIQUE_KEY_RECHECK;
	trigger.tgtype = TRIGGER_TYPE_ROW | TRIGGER_TYPE_AFTER | TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE;
	trigger.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trigger.tgisinternal = true;
	trigger.tgdeferrable = true;
	trigger.tginitdeferred = con->condeferred;
	tuple = provider_fixed(rows->relations[slot], &trigger, sizeof(trigger), Anum_pg_trigger_tgnargs);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgattr, PointerGetDatum(buildint2vector(NULL, 0)));
	SET_VARSIZE(args, VARHDRSZ);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_trigger_tgargs, PointerGetDatum(args));
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
	provider_dependency(rows, TriggerRelationId, trigger.oid, 0, ConstraintRelationId, con->oid, 0, DEPENDENCY_INTERNAL);
	foreach_ptr(HeapTupleData, klass, rows->rows[provider_catalog_index(RelationRelationId)])
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(klass);

		if (form->oid == trigger.tgrelid)
			form->relhastriggers = true;
	}
}

static HeapTuple
provider_foreign(DGProviderRows *rows, CatalogView *view, HeapTuple tuple,
				 Constraint *constraint, TupleDesc desc, const char *metadata,
				 int count, char **definitions, char **kinds)
{
	int			slot = provider_catalog_index(ConstraintRelationId);
	Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
	TupleDesc	referenced = NULL;
	char	   *name;
	Datum		keys[INDEX_MAX_KEYS];
	int			k = 0;
	const int	fields[] = {Anum_pg_constraint_conpfeqop, Anum_pg_constraint_conppeqop, Anum_pg_constraint_conffeqop};
	const char *labels[] = {"pf", "pp", "ff"};

	if (!metadata || !constraint->pktable->schemaname)
		elog(ERROR, "foreign key metadata requires qualified identities");
	name = dg_qualified_name(constraint->pktable->schemaname, constraint->pktable->relname);
	for (int i = 0; i < count; i++)
		if (!strcmp(kinds[i], "table") && !strcmp(json_value(definitions[i], "name"), name))
			referenced = provider_columns(definitions[i]);
	if (!referenced)
		elog(ERROR, "canonical referenced table is missing");
	con->confrelid = CatalogViewLookupObject(view, RelationRelationId, name);
	con->conindid = CatalogViewLookupObject(view, RelationRelationId, json_value(metadata, "index"));
	con->confupdtype = constraint->fk_upd_action;
	con->confdeltype = constraint->fk_del_action;
	con->confmatchtype = constraint->fk_matchtype;
	foreach_ptr(String, column, constraint->pk_attrs)
	{
		int			att = provider_attribute(referenced, strVal(column));

		if (k >= INDEX_MAX_KEYS)
			elog(ERROR, "too many foreign key columns");
		keys[k++] = Int16GetDatum(att);
		provider_dependency(rows, ConstraintRelationId, con->oid, 0, RelationRelationId, con->confrelid, att, DEPENDENCY_NORMAL);
	}
	provider_dependency(rows, ConstraintRelationId, con->oid, 0, RelationRelationId, con->conindid, 0, DEPENDENCY_NORMAL);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_confkey, PointerGetDatum(construct_array(keys, k, INT2OID, 2, true, TYPALIGN_SHORT)));
	for (int i = 0; i < lengthof(fields); i++)
	{
		Datum		operators[INDEX_MAX_KEYS];

		run("SELECT jsonb_array_elements_text($1::jsonb->$2)", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(metadata), CStringGetTextDatum(labels[i])), NULL, true);
		if (SPI_processed != k)
			elog(ERROR, "foreign key operator arity mismatch");
		for (int j = 0; j < k; j++)
			operators[j] = DirectFunctionCall1(regoperatorin, CStringGetDatum(strcell(j, 1)));
		tuple = provider_set(rows->relations[slot], tuple, fields[i], PointerGetDatum(construct_array(operators, k, OIDOID, sizeof(Oid), true, TYPALIGN_INT)));
	}
	if (constraint->fk_del_set_cols)
	{
		k = 0;
		foreach_ptr(String, column, constraint->fk_del_set_cols)
			keys[k++] = Int16GetDatum(provider_attribute(desc, strVal(column)));
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_confdelsetcols, PointerGetDatum(construct_array(keys, k, INT2OID, 2, true, TYPALIGN_SHORT)));
	}
	con = (Form_pg_constraint) GETSTRUCT(tuple);
	for (int i = 0; i < 4; i++)
		provider_foreign_trigger(rows, view, con, i);
	return tuple;
}
