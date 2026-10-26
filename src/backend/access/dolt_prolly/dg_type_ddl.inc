/*
 * Versioned user-defined types. Type DDL in a versioned database runs natively
 * against a provisional copy of the branch's catalogs; every user type there
 * is then encoded again and published. Unchanged types encode to the same
 * bytes.
 */

/* Whether a type DDL statement creates a type in a versioned schema. */
static bool
creates_versioned_type(Node *statement)
{
	Oid			namespace;
	char	   *name;

	if (IsA(statement, CreateEnumStmt))
		namespace = QualifiedNameGetCreationNamespace(((CreateEnumStmt *) statement)->typeName, &name);
	else if (IsA(statement, CreateDomainStmt))
		namespace = QualifiedNameGetCreationNamespace(((CreateDomainStmt *) statement)->domainname, &name);
	else if (IsA(statement, CompositeTypeStmt))
		namespace = RangeVarGetCreationNamespace(((CompositeTypeStmt *) statement)->typevar);
	else
		return false;
	return !isAnyTempNamespace(namespace) && !dg_system_namespace(get_namespace_name(namespace));
}

/* Whether a type DDL statement changes a versioned type. */
static bool
alters_versioned_type(Node *statement)
{
	Oid			type = InvalidOid;

	if (IsA(statement, AlterEnumStmt))
		type = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterEnumStmt *) statement)->typeName));
	else if (IsA(statement, AlterDomainStmt))
		type = typenameTypeId(NULL, makeTypeNameFromNameList(((AlterDomainStmt *) statement)->typeName));
	else if (IsA(statement, RenameStmt) &&
			 (((RenameStmt *) statement)->renameType == OBJECT_TYPE ||
			  ((RenameStmt *) statement)->renameType == OBJECT_DOMAIN))
		type = typenameTypeId(NULL, makeTypeNameFromNameList(castNode(List, ((RenameStmt *) statement)->object)));
	else if (IsA(statement, RenameStmt) && ((RenameStmt *) statement)->renameType == OBJECT_ATTRIBUTE)
		type = typenameTypeId(NULL, makeTypeNameFromNameList(((RenameStmt *) statement)->relation->schemaname ?
															 list_make2(makeString(((RenameStmt *) statement)->relation->schemaname),
																		makeString(((RenameStmt *) statement)->relation->relname)) :
															 list_make1(makeString(((RenameStmt *) statement)->relation->relname))));
	return type >= FirstDurableObjectId;
}

/* Whether a pg_type row is a user type of its own, not a derived array or row type. */
static bool
type_versioned_row(Form_pg_type type)
{
	if (type->oid < FirstNormalObjectId)
		return false;
	if (type->typtype == TYPTYPE_ENUM || type->typtype == TYPTYPE_DOMAIN)
		return true;
	return type->typtype == TYPTYPE_COMPOSITE && get_rel_relkind(type->typrelid) == RELKIND_COMPOSITE_TYPE;
}

/*
 * Whether anything but a type's own array type and composite relation depends
 * on it. Definitions name the types they use, so renaming a used type would
 * leave them naming a type that no longer exists.
 */
static bool
type_in_use(Oid type)
{
	Relation	catalog = table_open(DependRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tuple;
	Oid			array = get_array_type(type);
	Oid			relation = typeOrDomainTypeRelid(type);
	bool		used = false;

	while (!used && HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tuple);

		if (dep->refclassid != TypeRelationId || (dep->refobjid != type && dep->refobjid != array))
			continue;
		if ((dep->classid == TypeRelationId && dep->objid == array) ||
			(dep->classid == RelationRelationId && dep->objid == relation && dep->objsubid == 0))
			continue;
		used = true;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return used;
}

/* Type DDL makes no storage; a composite type's relation has none. */
static void
type_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE)
		unsupported("storage effects of type DDL");
}

static void
type_ddl(ParseState *pstate, Node *statement)
{
	CatalogView *view = CatalogViewCreate("provisional type DDL");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
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

		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		current = capture(true);
		provider_populate(view, current.hash, NULL);
		end_api(nest);
		CatalogViewBeginMutation(view);
		CatalogViewSetStorageHandler(view, type_storage);

		/* Domain constraints validate against the branch's existing rows. */
		native_mutation_view = view;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		switch (nodeTag(statement))
		{
			case T_CreateEnumStmt:
				DefineEnum((CreateEnumStmt *) statement);
				break;
			case T_CreateDomainStmt:
				DefineDomain(pstate, (CreateDomainStmt *) statement);
				break;
			case T_CompositeTypeStmt:
				DefineCompositeType(((CompositeTypeStmt *) statement)->typevar,
									((CompositeTypeStmt *) statement)->coldeflist);
				break;
			case T_AlterEnumStmt:

				/*
				 * Stored rows hold enum labels, which keep them portable; a
				 * renamed label would no longer read back.
				 */
				if (((AlterEnumStmt *) statement)->oldVal)
					unsupported("renaming a label of a versioned enum");
				AlterEnum((AlterEnumStmt *) statement);
				break;
			case T_AlterDomainStmt:
				{
					AlterDomainStmt *stmt = (AlterDomainStmt *) statement;

					switch (stmt->subtype)
					{
						case 'T':
							AlterDomainDefault(stmt->typeName, stmt->def);
							break;
						case 'N':
							AlterDomainNotNull(stmt->typeName, false);
							break;
						case 'O':
							AlterDomainNotNull(stmt->typeName, true);
							break;
						case 'C':
							AlterDomainAddConstraint(stmt->typeName, stmt->def, NULL, false);
							break;
						case 'X':
							AlterDomainDropConstraint(stmt->typeName, stmt->name, stmt->behavior, stmt->missing_ok);
							break;
						case 'V':
							AlterDomainValidateConstraint(stmt->typeName, stmt->name);
							break;
						default:
							elog(ERROR, "unrecognized ALTER DOMAIN subtype %c", stmt->subtype);
					}
					break;
				}
			default:
				if (((RenameStmt *) statement)->renameType != OBJECT_ATTRIBUTE &&
					type_in_use(typenameTypeId(NULL, makeTypeNameFromNameList(castNode(List, ((RenameStmt *) statement)->object)))))
					unsupported("renaming a versioned type that other objects use");
				ExecRenameStmt((RenameStmt *) statement);
				break;
		}
		CommandCounterIncrement();
		GetUserIdAndSecContext(&privileged, &security);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
		nest = begin_api();
		catalog = table_open(TypeRelationId, AccessShareLock);
		scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			if (type_versioned_row((Form_pg_type) GETSTRUCT(tuple)))
				definitions = lappend(definitions, type_encode(tuple, RelationGetDescr(catalog)));
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
		CatalogViewSwitch(NULL);
		(void) objects_publish("type", NIL, NULL, true, definitions);
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
dg_catalog_type_ddl(ParseState *pstate, Node *statement)
{
	bool		pushed = false;

	if (!IsA(statement, CreateEnumStmt) && !IsA(statement, CreateDomainStmt) &&
		!IsA(statement, CompositeTypeStmt) && !IsA(statement, AlterEnumStmt) &&
		!IsA(statement, AlterDomainStmt) && !IsA(statement, RenameStmt))
		return false;
	if (!dg_catalog_active() || !(creates_versioned_type(statement) || alters_versioned_type(statement)))
		return false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (CatalogViewCurrent())
		unsupported("type DDL in an immutable revision context");
	dg_storage_lock_working(dg_current_branch(), GetActiveSnapshot());
	type_ddl(pstate, statement);
	if (pushed)
		PopActiveSnapshot();
	return true;
}
