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
				elog(ERROR, "unrecognized versioned type statement");
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
		!IsA(statement, AlterDomainStmt))
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
