/*
 * Renames of versioned objects. Definitions name the objects they use, so a
 * rename runs natively against a provisional copy of the branch's catalogs,
 * where every object still refers to the renamed one by OID, and then
 * encodes again from that copy exactly the renamed object and the members
 * the database's dependency record lists as referring to it. Tables and
 * indexes keep their rows and storage under their new names.
 */

/* A versioned object's definition, encoded again from provisional |view|. */
static char *
rename_encode(CatalogView *view, const char *kind, const char *name, const char *old)
{
	CatalogView *previous = CatalogViewSwitch(view);
	char	   *result = NULL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		if (!strcmp(kind, "table") || !strcmp(kind, "index"))
		{
			Oid			relid = CatalogViewLookupObject(view, RelationRelationId, name);
			int		   *mapping;
			int			count;

			result = mutation_columns(view, relid, old, &mapping, &count);
			if (!strcmp(kind, "table"))
			{
				result = mutation_constraints(view, relid, result);
				result = mutation_index_list(result, mutation_indexes(view, relid));
			}
			else
			{
				char	   *sql;

				schema_expression_gucs();
				sql = TextDatumGetCString(DirectFunctionCall1(pg_get_indexdef, ObjectIdGetDatum(relid)));
				run("SELECT jsonb_set($1::jsonb,'{index_definition}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(sql)), NULL, true);
				result = strcell(0, 1);
			}
			run("SELECT jsonb_set($1::jsonb,'{name}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID),
				VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(dg_qualified_name(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid)))), NULL, true);
			result = strcell(0, 1);
		}
		else if (!strcmp(kind, "routine"))
		{
			HeapTuple	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(CatalogViewLookupObject(view, ProcedureRelationId, name)));
			Relation	catalog = table_open(ProcedureRelationId, AccessShareLock);

			result = routine_encode(tuple, RelationGetDescr(catalog));
			table_close(catalog, AccessShareLock);
			ReleaseSysCache(tuple);
		}
		else if (!strcmp(kind, "type"))
		{
			HeapTuple	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(CatalogViewLookupObject(view, TypeRelationId, name)));
			Relation	catalog = table_open(TypeRelationId, AccessShareLock);

			result = type_encode(tuple, RelationGetDescr(catalog));
			table_close(catalog, AccessShareLock);
			ReleaseSysCache(tuple);
		}
		else if (!strcmp(kind, "trigger"))
		{
			Oid			oid = CatalogViewLookupObject(view, TriggerRelationId, name);
			Relation	catalog = table_open(TriggerRelationId, AccessShareLock);
			ScanKeyData key;
			SysScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(oid));
			scan = systable_beginscan(catalog, TriggerOidIndexId, true, NULL, 1, &key);
			tuple = systable_getnext(scan);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "versioned trigger %s is missing", name);
			result = trigger_encode(tuple, RelationGetDescr(catalog));
			systable_endscan(scan);
			table_close(catalog, AccessShareLock);
		}
		else
			elog(ERROR, "cannot encode %s %s again", kind, name);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

static bool
rename_targeted(List *targets, const char *kind, const char *name)
{
	foreach_ptr(List, target, targets)
		if (!strcmp(linitial(target), kind) && !strcmp(lsecond(target), name))
		return true;
	return false;
}

/*
 * Encode |targets|, (kind, name) pairs naming members of the working schema
 * |current|, again from |view| and publish them in place of the old ones.
 */
static void
rename_publish(CatalogView *view, Capture current, List *targets)
{
	VersionRef	working = read_ref(branch_ref("working"));
	StringInfoData json;
	Datum	   *children;
	bytea	  **members;
	char	  **kinds,
			  **names;
	int			count,
				used = 0;

	run("SELECT t.objhash,t.objkind,CASE WHEN t.objkind<>'namespace' THEN convert_from(t.objpayload,'UTF8')::jsonb->>'name' END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
	count = SPI_processed;
	members = palloc_array(bytea *, Max(count, 1));
	kinds = palloc_array(char *, Max(count, 1));
	names = palloc_array(char *, Max(count, 1));
	children = palloc_array(Datum, Max(count, 1));
	for (int i = 0; i < count; i++)
	{
		members[i] = DatumGetByteaPCopy(cell(i, 1));
		kinds[i] = strcell(i, 2);
		names[i] = strcell(i, 3);
	}
	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	for (int i = 0; i < count; i++)
	{
		char	   *payload = metadata(members[i], kinds[i]);
		bytea	   *member = members[i];

		if (names[i] && rename_targeted(targets, kinds[i], names[i]))
		{
			bytea	   *oldschema = child(members[i], 1);
			char	   *definition = rename_encode(view, kinds[i], names[i], metadata(oldschema, "schema"));
			char	   *name = json_value(definition, "name");

			if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
			{
				bytea	   *schema = put("schema", definition, NULL, 0);
				Oid			resource = binding_relation(dg_current_branch(), names[i], oldschema);

				if (!OidIsValid(resource))
					elog(ERROR, "renamed relation's storage binding is unavailable");
				run("SELECT jsonb_set(jsonb_set($1::jsonb,'{name}',to_jsonb($2::text)),'{schema}',to_jsonb(encode(sha256(convert_to($3,'UTF8')),'hex')))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID),
					VC_ARGS(CStringGetTextDatum(payload), CStringGetTextDatum(name), CStringGetTextDatum(definition)), NULL, true);
				payload = strcell(0, 1);
				member = put(kinds[i], payload, VC_ARGS(PointerGetDatum(schema), PointerGetDatum(child(members[i], 2))), 2);
				binding_record(dg_current_branch(), name, schema, resource);
			}
			else
			{
				payload = object_payload(kinds[i], definition);
				member = object_member(kinds[i], definition);
			}
		}
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(kinds[i], "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payload);
		else
			appendStringInfoString(&json, payload);
		children[used++] = PointerGetDatum(member);
	}
	appendStringInfoChar(&json, ']');
	run("SELECT jsonb_set($1::jsonb,'{members}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(json.data)), NULL, true);
	move_ref(branch_ref("working"), working, canonical_database(strcell(0, 1), children, used));
	advance_snapshot();
}

/* Add the members that refer to |kind| object |name| to |targets|. */
static List *
rename_referrers(List *targets, const char *json, const char *kind, const char *name)
{
	foreach_ptr(List, referrer, database_referrers(json, kind, name))
		if (!rename_targeted(targets, linitial(referrer), lsecond(referrer)))
		targets = lappend(targets, referrer);
	return targets;
}

/*
 * The versioned object a rename statement renames, in provisional |view|:
 * its kind and old name, and the references by which members name it.
 * Returns false if the statement renames no versioned object.
 */
static bool
rename_subject(Node *statement, char **kind, char **name, List **references)
{
	ObjectType	type;
	Node	   *object = NULL;
	RangeVar   *relation = NULL;

	if (IsA(statement, RenameStmt))
	{
		type = ((RenameStmt *) statement)->renameType;
		object = ((RenameStmt *) statement)->object;
		relation = ((RenameStmt *) statement)->relation;
		/* A column or constraint rename changes its table's or domain's definition. */
		if (type == OBJECT_COLUMN)
			type = ((RenameStmt *) statement)->relationType;
		else if (type == OBJECT_TABCONSTRAINT)
			type = OBJECT_TABLE;
		else if (type == OBJECT_DOMCONSTRAINT)
			type = OBJECT_DOMAIN;
	}
	else if (IsA(statement, AlterObjectSchemaStmt))
	{
		type = ((AlterObjectSchemaStmt *) statement)->objectType;
		object = ((AlterObjectSchemaStmt *) statement)->object;
		relation = ((AlterObjectSchemaStmt *) statement)->relation;
	}
	else if (IsA(statement, AlterEnumStmt) && ((AlterEnumStmt *) statement)->oldVal)
	{
		Oid			id = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterEnumStmt *) statement)->typeName));

		if (id < FirstDurableObjectId)
			return false;
		*kind = "type";
		*name = CatalogViewObjectIdentity(id, TypeRelationId);
		*references = list_make1(list_make2("type", *name));
		return true;
	}
	else
		return false;
	*references = NIL;
	switch (type)
	{
		case OBJECT_TABLE:
		case OBJECT_INDEX:
			{
				Oid			relid = RangeVarGetRelid(relation, NoLock, true);

				if (!OidIsValid(relid) || relid < FirstDurableObjectId)
					return false;
				*kind = get_rel_relkind(relid) == RELKIND_INDEX ? "index" : "table";
				*name = CatalogViewObjectIdentity(relid, RelationRelationId);
				if (!strcmp(*kind, "index"))
				{
					/* The table's definition lists its indexes. */
					Oid			table = IndexGetRelation(relid, false);

					*references = list_make1(list_make2("table", CatalogViewObjectIdentity(table, RelationRelationId)));
				}
				else
				{
					*references = list_make2(list_make2("table", *name), list_make2("type", *name));
				}
				return true;
			}
		case OBJECT_TRIGGER:
			{
				Oid			relid = RangeVarGetRelid(relation, NoLock, true);

				if (!OidIsValid(relid) || relid < FirstDurableObjectId)
					return false;
				*kind = "trigger";
				*name = psprintf("%s/%s", CatalogViewObjectIdentity(relid, RelationRelationId),
								 ((RenameStmt *) statement)->subname);
				return true;
			}
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			{
				Oid			id = LookupFuncWithArgs(type, castNode(ObjectWithArgs, object), true);

				if (!OidIsValid(id) || id < FirstDurableObjectId)
					return false;
				*kind = "routine";
				*name = CatalogViewObjectIdentity(id, ProcedureRelationId);
				*references = list_make1(list_make2("routine", dg_qualified_name(get_namespace_name(get_func_namespace(id)), get_func_name(id))));
				return true;
			}
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_ATTRIBUTE:
			{
				Oid			id;

				if (type == OBJECT_ATTRIBUTE)
					id = typenameTypeId(NULL, makeTypeNameFromNameList(relation->schemaname ?
																	   list_make2(makeString(relation->schemaname), makeString(relation->relname)) :
																	   list_make1(makeString(relation->relname))));
				else
					id = typenameTypeId(NULL, makeTypeNameFromNameList(castNode(List, object)));
				if (id < FirstDurableObjectId)
					return false;
				*kind = "type";
				*name = CatalogViewObjectIdentity(id, TypeRelationId);
				*references = list_make1(list_make2("type", *name));
				return true;
			}
		default:
			return false;
	}
}

typedef struct RenameLabel
{
	TupleDesc	desc;
	Oid			type;
	const char *old;
	const char *renamed;
} RenameLabel;

static PLSlice
rename_label_field(int field, PLSlice bytes, void *arg)
{
	RenameLabel *label = arg;

	return dg_recode_label(TupleDescAttr(label->desc, field)->atttypid, bytes, label->type, label->old, label->renamed);
}

/*
 * Rows hold enum labels: rewrite the rows of every table that can hold a
 * value of enum |type|, directly or through domains, arrays and composite
 * types, as the dependency record of |root| lists them.
 */
static void
rename_label_rows(const char *root, const char *type, const char *old, const char *renamed)
{
	List	   *types = list_make1(pstrdup(type));
	List	   *tables = NIL;

	foreach_ptr(char, name, types)
	{
		foreach_ptr(List, referrer, database_referrers(root, "type", name))
		{
			List	  **into = !strcmp(linitial(referrer), "type") ? &types :
				!strcmp(linitial(referrer), "table") ? &tables : NULL;
			bool		listed = false;

			if (!into)
				continue;
			foreach_ptr(char, member, *into)
				listed |= !strcmp(member, lsecond(referrer));
			if (!listed)
				*into = lappend(*into, lsecond(referrer));
		}
	}
	foreach_ptr(char, table, tables)
	{
		Relation	rel = dg_catalog_write_table(dg_current_branch(), table, ACL_UPDATE);
		Oid			target = CatalogViewLookupObject(rel->rd_catalogview, TypeRelationId, type);
		DGState    *state = dg_load(rel, SnapshotSelf);
		TupleDesc	desc = RelationGetDescr(rel);
		int		   *mapping = palloc_array(int, Max(desc->natts, 1));
		RenameLabel label = {
			.desc = desc,
			.type = target,
			.old = old,
			.renamed = renamed,
		};
		PLTree		tree;

		for (int i = 0; i < desc->natts; i++)
			mapping[i] = i;
		tree = alter_rows(state, desc->natts, desc->natts, mapping, rename_label_field, &label);
		dg_history_install(rel, state, tree);
		dg_release(state);
		dg_storage_close_relation(rel);
		UpdateActiveSnapshotCommandId();
	}
	if (tables)
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
	advance_snapshot();
}

static void
rename_storage(Relation relation, CatalogViewStorageEffect effect)
{
	unsupported("storage effects of a rename");
}

bool
dg_catalog_rename(ParseState *pstate, Node *statement)
{
	CatalogView *view,
			   *previous;
	CatalogView *saved_mutation = native_mutation_view;
	Oid			caller;
	int			security,
				nest;
	bool		pushed = false;
	char	   *kind,
			   *name;
	List	   *references;

	if ((!IsA(statement, RenameStmt) && !IsA(statement, AlterObjectSchemaStmt) && !IsA(statement, AlterEnumStmt)) ||
		!dg_catalog_active())
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}

	/* The subject is resolved as the statement names it, on the branch head. */
	if (!rename_subject(statement, &kind, &name, &references))
	{
		if (pushed)
			PopActiveSnapshot();
		return false;
	}
	if (CatalogViewCurrent())
		unsupported("rename in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	view = CatalogViewCreate("provisional rename");
	previous = CatalogViewCurrent();
	GetUserIdAndSecContext(&caller, &security);
	PG_TRY();
	{
		Capture		current;
		List	   *targets;
		char	   *root;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		CommandCounterIncrement();
		UpdateActiveSnapshotCommandId();
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, rename_storage);
		native_mutation_view = view;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		if (IsA(statement, RenameStmt))
			ExecRenameStmt((RenameStmt *) statement);
		else if (IsA(statement, AlterEnumStmt))
			AlterEnum((AlterEnumStmt *) statement);
		else
			ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) statement, NULL);
		CommandCounterIncrement();
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		CatalogViewSwitch(NULL);

		/*
		 * The working schema is unchanged since the copy was made: native
		 * DDL changed only the copy.
		 */
		nest = begin_api();
		current = capture(true);
		CommandCounterIncrement();
		UpdateActiveSnapshotCommandId();
		root = metadata(current.hash, "database");
		targets = list_make1(list_make2(kind, name));
		foreach_ptr(List, reference, references)
		{
			if (!strcmp(linitial(reference), "table") && strcmp(kind, "table"))
			{
				/* An index's table lists it; the table itself is re-encoded. */
				if (!rename_targeted(targets, "table", lsecond(reference)))
					targets = lappend(targets, reference);
			}
			targets = rename_referrers(targets, root, linitial(reference), lsecond(reference));
		}
		rename_publish(view, current, targets);
		if (IsA(statement, AlterEnumStmt))
			rename_label_rows(root, name, ((AlterEnumStmt *) statement)->oldVal,
							  ((AlterEnumStmt *) statement)->newVal);
		end_api(nest);
		UpdateActiveSnapshotCommandId();
	}
	PG_FINALLY();
	{
		native_mutation_view = saved_mutation;
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}
