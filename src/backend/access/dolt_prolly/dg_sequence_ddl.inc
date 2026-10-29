/* Native CREATE validates the definition in a provisional catalog. Only the
 * portable definition is published; the allocator is shared by logical name. */
static void
sequence_storage_effect(Relation relation, CatalogViewStorageEffect effect)
{
	if (effect != CATALOG_STORAGE_CREATE || relation->rd_rel->relkind != RELKIND_SEQUENCE)
		unsupported("sequence storage mutation");
}

bool
dg_catalog_sequence_ddl(ParseState *pstate, Node *statement)
{
	CreateSeqStmt *stmt;
	RangeVar   *rv;
	bool		creating;
	Oid			ns,
				caller,
				existing;
	int			security,
				nest;
	bool		pushed = false;
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *view;

	if ((!IsA(statement, CreateSeqStmt) && !IsA(statement, AlterSeqStmt)) || catalog_api_depth)
		return false;
	creating = IsA(statement, CreateSeqStmt);
	stmt = creating ? copyObject((CreateSeqStmt *) statement) : NULL;
	rv = creating ? stmt->sequence : ((AlterSeqStmt *) statement)->sequence;
	if (creating && rv->relpersistence != RELPERSISTENCE_PERMANENT)
		return false;
	if (!dg_catalog_active())
	{
		if (!default_table_access_method || strcmp(default_table_access_method, "dolt_prolly_table"))
			return false;
		catalog_activate();
	}
	if (!creating && !CatalogViewIsDurable(RangeVarGetRelid(rv, NoLock, true)))
		return false;
	if (previous)
		unsupported("sequence DDL in immutable catalog context");
	if (!creating)
	{
		foreach_node(DefElem, option, ((AlterSeqStmt *) statement)->options)
			if (strcmp(option->defname, "owned_by"))
				unsupported("versioned ALTER SEQUENCE options other than OWNED BY");
	}
	if (creating)
		ns = RangeVarGetAndCheckCreationNamespace(rv, NoLock, &existing);
	else
	{
		existing = RangeVarGetRelid(rv, AccessExclusiveLock, false);
		ns = get_rel_namespace(existing);
	}
	if (creating && OidIsValid(existing))
	{
		if (!stmt->if_not_exists)
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE), errmsg("relation \"%s\" already exists", stmt->sequence->relname)));
		ereport(NOTICE, (errmsg("relation \"%s\" already exists, skipping", stmt->sequence->relname)));
		return true;
	}
	if (isAnyTempNamespace(ns) || dg_system_namespace(get_namespace_name(ns)))
		return false;
	if (creating)
		rv->schemaname = pstrdup(get_namespace_name(ns));
	adopt_schema(ns);
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	GetUserIdAndSecContext(&caller, &security);
	view = CatalogViewCreate("provisional sequence CREATE");
	PG_TRY();
	{
		Capture		current;
		ObjectAddress address;
		char	   *definition = NULL;

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, dg_current_branch());
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, sequence_storage_effect);
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		if (creating)
		{
			/* Recheck after taking the branch publication lock. A concurrent
			 * CREATE may have committed after the initial namespace lookup. */
			existing = CatalogViewFindObject(view, RelationRelationId,
										 dg_qualified_name(rv->schemaname, rv->relname));
			if (OidIsValid(existing))
			{
				if (!stmt->if_not_exists)
					ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE), errmsg("relation \"%s\" already exists", rv->relname)));
				ereport(NOTICE, (errmsg("relation \"%s\" already exists, skipping", rv->relname)));
				ObjectAddressSet(address, RelationRelationId, InvalidOid);
			}
			else
				address = DefineSequence(pstate, stmt);
		}
		else
		{
			Oid			id = RangeVarGetRelid(rv, NoLock, false);
			Relation	rel = sequence_open(id, NoLock);

			if (!object_ownercheck(RelationRelationId, id, caller))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SEQUENCE, RelationGetRelationName(rel));
			foreach_node(DefElem, option, ((AlterSeqStmt *) statement)->options)
				SequenceSetOwnedBy(rel, castNode(List, option->arg), ((AlterSeqStmt *) statement)->for_identity);
			sequence_close(rel, NoLock);
			ObjectAddressSet(address, RelationRelationId, id);
		}
		CommandCounterIncrement();
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		if (OidIsValid(address.objectId))
		{
			CatalogViewSwitch(view);
			definition = sequence_encode(address.objectId);
			CatalogViewSwitch(NULL);
			if (!creating)
				definition = sequence_capture(definition);
			{
				Oid			resource = sequence_resource(json_value(definition, "name"), sequence_number(definition, "start"), sequence_number(definition, "increment"), true);
				bytea	   *schema = put("schema", definition, NULL, 0);

				binding_record(dg_current_branch(), json_value(definition, "name"), schema, resource);
			}
			objects_publish("sequence", creating ? NIL : list_make1(json_value(definition, "name")), NULL, false, list_make1(definition));
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(caller, security);
		CatalogViewSwitch(previous);
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
		if (pushed)
			PopActiveSnapshot();
	}
	PG_END_TRY();
	return true;
}
