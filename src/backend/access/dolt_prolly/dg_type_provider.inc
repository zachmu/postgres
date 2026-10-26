/*
 * Rebuild versioned user-defined types. All of a schema's types, and their
 * array types, are bound before any rows are built, as types, tables and
 * routines name one another; arrays are found through |rows->arrays|.
 */
static void
provider_type_bind(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	char	   *array = json_value(definition, "array");
	Oid			type = CatalogViewBindObject(view, TypeRelationId, name, InvalidOid);

	if (array)
	{
		char	   *schema = strVal(linitial(stringToQualifiedNameList(name, NULL)));

		rows->elements = lappend_oid(rows->elements, type);
		rows->arrays = lappend_oid(rows->arrays, CatalogViewBindObject(view, TypeRelationId, dg_qualified_name(schema, array), InvalidOid));
	}
	if (!strcmp(json_value(definition, "kind"), "c"))
		(void) CatalogViewBindObject(view, RelationRelationId, name, InvalidOid);
}

/* The array type of |element| among the types being rebuilt, if any. */
static Oid
provider_array_of(DGProviderRows *rows, Oid element)
{
	ListCell   *e,
			   *a;

	forboth(e, rows->elements, a, rows->arrays)
	{
		if (lfirst_oid(e) == element)
			return lfirst_oid(a);
	}
	return InvalidOid;
}

static Oid
provider_collation(const char *name)
{
	return name ? get_collation_oid(stringToQualifiedNameList(name, NULL), false) : InvalidOid;
}

static void
provider_type_array(DGProviderRows *rows, Form_pg_type type, const char *arrayname)
{
	FormData_pg_type array = {0};

	array.oid = type->typarray;
	namestrcpy(&array.typname, arrayname);
	array.typnamespace = type->typnamespace;
	array.typowner = type->typowner;
	array.typlen = -1;
	array.typtype = TYPTYPE_BASE;
	array.typcategory = TYPCATEGORY_ARRAY;
	array.typisdefined = true;
	array.typdelim = type->typdelim;
	array.typelem = type->oid;
	array.typinput = F_ARRAY_IN;
	array.typoutput = F_ARRAY_OUT;
	array.typreceive = F_ARRAY_RECV;
	array.typsend = F_ARRAY_SEND;
	array.typanalyze = F_ARRAY_TYPANALYZE;
	array.typsubscript = F_ARRAY_SUBSCRIPT_HANDLER;
	array.typalign = type->typalign == TYPALIGN_DOUBLE ? TYPALIGN_DOUBLE : TYPALIGN_INT;
	array.typstorage = TYPSTORAGE_EXTENDED;
	array.typtypmod = -1;
	array.typcollation = type->typcollation;
	provider_add(rows, TypeRelationId, &array, sizeof(array), Anum_pg_type_typcollation);
	provider_dependency(rows, TypeRelationId, array.oid, 0, TypeRelationId, type->oid, 0, DEPENDENCY_INTERNAL);
}

static void
provider_type(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	int			slot = provider_catalog_index(TypeRelationId);
	TupleDesc	desc = RelationGetDescr(rows->relations[slot]);
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Datum		values[Natts_pg_type] = {0};
	bool		nulls[Natts_pg_type] = {0};
	HeapTuple	tuple;
	Form_pg_type type;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		char	   *encoded = json_value(definition, NameStr(attr->attname));
		Oid			input,
					param;

		switch (i + 1)
		{
			case Anum_pg_type_oid:
				values[i] = ObjectIdGetDatum(CatalogViewFindObject(view, TypeRelationId, name));
				continue;
			case Anum_pg_type_typname:
				values[i] = DirectFunctionCall1(namein, CStringGetDatum(strVal(lsecond(parts))));
				continue;
			case Anum_pg_type_typrelid:
				values[i] = ObjectIdGetDatum(!strcmp(json_value(definition, "kind"), "c") ?
											 CatalogViewFindObject(view, RelationRelationId, name) : InvalidOid);
				continue;
			case Anum_pg_type_typarray:
				values[i] = ObjectIdGetDatum(provider_array_of(rows, CatalogViewFindObject(view, TypeRelationId, name)));
				continue;
			case Anum_pg_type_typacl:
			case Anum_pg_type_typdefault:
			case Anum_pg_type_typdefaultbin:
				/* A domain's default is bound with its constraints. */
				nulls[i] = true;
				continue;
		}
		if (!encoded)
		{
			if (attr->atttypid == OIDOID || attr->atttypid == REGPROCOID)
				values[i] = ObjectIdGetDatum(InvalidOid);
			else
				nulls[i] = true;
			continue;
		}
		switch (i + 1)
		{
			case Anum_pg_type_typnamespace:
				values[i] = ObjectIdGetDatum(CatalogViewLookupObject(view, NamespaceRelationId, encoded));
				break;
			case Anum_pg_type_typowner:
				values[i] = ObjectIdGetDatum(get_role_oid(encoded, false));
				break;
			case Anum_pg_type_typelem:
			case Anum_pg_type_typbasetype:
				values[i] = ObjectIdGetDatum(routine_type_oid(rows, view, encoded));
				break;
			case Anum_pg_type_typcollation:
				values[i] = ObjectIdGetDatum(provider_collation(encoded));
				break;
			default:
				getTypeInputInfo(attr->atttypid, &input, &param);
				values[i] = OidInputFunctionCall(input, encoded, param, -1);
				break;
		}
	}
	tuple = heap_form_tuple(desc, values, nulls);
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
	type = (Form_pg_type) GETSTRUCT(tuple);
	provider_dependency(rows, TypeRelationId, type->oid, 0, NamespaceRelationId, type->typnamespace, 0, DEPENDENCY_NORMAL);
	if (OidIsValid(type->typbasetype))
		provider_dependency(rows, TypeRelationId, type->oid, 0, TypeRelationId, type->typbasetype, 0, DEPENDENCY_NORMAL);
	if (OidIsValid(type->typarray))
		provider_type_array(rows, type, json_value(definition, "array"));

	if (type->typtype == TYPTYPE_ENUM)
	{
		/* Labels are bound in sort order, so even OIDs compare in label order. */
		run("SELECT l FROM jsonb_array_elements_text($1::jsonb->'labels') WITH ORDINALITY a(l,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
		for (uint64 j = 0; j < SPI_processed; j++)
		{
			FormData_pg_enum label = {0};
			char	   *text = strcell(j, 1);

			label.oid = CatalogViewBindObject(view, EnumRelationId, psprintf("%s/%s", name, text), InvalidOid);
			label.enumtypid = type->oid;
			label.enumsortorder = j + 1;
			namestrcpy(&label.enumlabel, text);
			provider_add(rows, EnumRelationId, &label, sizeof(label), Anum_pg_enum_enumlabel);
		}
	}
}

/*
 * A composite type's relation and attributes, once every type's row exists:
 * an attribute's type may be any of them.
 */
static void
provider_type_relation(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	Form_pg_type type = provider_row_type(rows, CatalogViewFindObject(view, TypeRelationId, json_value(definition, "name")));

	if (type->typtype == TYPTYPE_COMPOSITE)
	{
		FormData_pg_class form = {0};
		int			natts;

		form.oid = type->typrelid;
		form.relnamespace = type->typnamespace;
		form.relname = type->typname;
		form.relowner = type->typowner;
		form.reltype = type->oid;
		form.relkind = RELKIND_COMPOSITE_TYPE;
		form.relpersistence = RELPERSISTENCE_PERMANENT;
		form.reltuples = -1;
		form.relispopulated = true;
		form.relreplident = REPLICA_IDENTITY_NOTHING;
		run("SELECT v->>'name',v->>'type',(v->>'typmod')::int,v->>'collation' FROM jsonb_array_elements($1::jsonb->'attributes') WITH ORDINALITY a(v,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
		natts = SPI_processed;
		form.relnatts = natts;
		{
			char	  **names = palloc_array(char *, Max(natts, 1));
			char	  **types = palloc_array(char *, Max(natts, 1));
			char	  **collations = palloc_array(char *, Max(natts, 1));
			int		   *mods = palloc_array(int, Max(natts, 1));

			for (int j = 0; j < natts; j++)
			{
				names[j] = strcell(j, 1);
				types[j] = strcell(j, 2);
				mods[j] = DatumGetInt32(cell(j, 3));
				collations[j] = strcell(j, 4);
			}
			provider_add(rows, RelationRelationId, &form, sizeof(form), Anum_pg_class_relminmxid);
			for (int j = 0; j < natts; j++)
			{
				TupleDesc	one = CreateTemplateTupleDesc(1);
				Oid			atttype = routine_type_oid(rows, view, types[j]);
				Form_pg_attribute attr;

				provider_init_entry(one, 1, names[j], atttype, mods[j]);
				attr = TupleDescAttr(one, 0);
				attr->attrelid = form.oid;
				attr->attnum = j + 1;
				attr->attislocal = true;
				if (collations[j])
					attr->attcollation = provider_collation(collations[j]);
				provider_add(rows, AttributeRelationId, attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
				provider_dependency(rows, RelationRelationId, form.oid, j + 1, TypeRelationId, atttype, 0, DEPENDENCY_NORMAL);
			}
		}
		/* As GenerateTypeDependencies: a stand-alone composite's relation belongs to its type. */
		provider_dependency(rows, RelationRelationId, form.oid, 0, TypeRelationId, type->oid, 0, DEPENDENCY_INTERNAL);
	}
}

/* Bind a domain's default and constraints, once routines can be named. */
static void
provider_domain(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	char	   *defaultsql = json_value(definition, "typdefaultbin");
	Oid			oid = CatalogViewFindObject(view, TypeRelationId, name);
	int			tslot = provider_catalog_index(TypeRelationId);
	int			cslot = provider_catalog_index(ConstraintRelationId);
	Form_pg_type type = NULL;
	ListCell   *cell_of_type = NULL;
	char	  **names,
			  **defs;
	bool	   *validated;
	int			n;

	foreach(cell_of_type, rows->rows[tslot])
	{
		type = (Form_pg_type) GETSTRUCT((HeapTuple) lfirst(cell_of_type));
		if (type->oid == oid)
			break;
		type = NULL;
	}
	if (!type)
		elog(ERROR, "canonical domain %s is missing", name);
	if (defaultsql)
	{
		Node	   *expression = schema_expression_bind_domain(view, schema_expression_raw(defaultsql), name,
															   type->typbasetype, type->typtypmod, type->typcollation, false);
		HeapTuple	tuple = (HeapTuple) lfirst(cell_of_type);

		tuple = provider_set(rows->relations[tslot], tuple, Anum_pg_type_typdefaultbin, CStringGetTextDatum(nodeToString(expression)));
		tuple = provider_set(rows->relations[tslot], tuple, Anum_pg_type_typdefault, CStringGetTextDatum(defaultsql));
		lfirst(cell_of_type) = tuple;
		type = (Form_pg_type) GETSTRUCT(tuple);
		provider_expression_dependencies(rows, TypeRelationId, oid, expression);
	}
	run("SELECT v->>0,v->>1,(v->>2)::bool FROM jsonb_array_elements($1::jsonb->'constraints') WITH ORDINALITY a(v,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	n = SPI_processed;
	names = palloc_array(char *, Max(n, 1));
	defs = palloc_array(char *, Max(n, 1));
	validated = palloc_array(bool, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		defs[i] = strcell(i, 2);
		validated[i] = DatumGetBool(cell(i, 3));
	}
	for (int i = 0; i < n; i++)
	{
		List	   *parsed = schema_expression_parse(psprintf("ALTER DOMAIN placeholder ADD CONSTRAINT %s %s", dg_quote_identifier(names[i]), defs[i]));
		AlterDomainStmt *stmt;
		Constraint *constraint;
		FormData_pg_constraint form = {0};
		Node	   *expression = NULL;
		HeapTuple	tuple;

		if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterDomainStmt))
			elog(ERROR, "invalid canonical domain constraint %s", names[i]);
		stmt = castNode(AlterDomainStmt, linitial_node(RawStmt, parsed)->stmt);
		constraint = castNode(Constraint, stmt->def);
		form.oid = CatalogViewBindObject(view, ConstraintRelationId, psprintf("%s/%s", name, names[i]), InvalidOid);
		namestrcpy(&form.conname, names[i]);
		form.connamespace = type->typnamespace;
		form.contypid = oid;
		form.conenforced = true;
		form.convalidated = validated[i];
		form.conislocal = true;
		form.confupdtype = ' ';
		form.confdeltype = ' ';
		form.confmatchtype = ' ';
		if (constraint->contype == CONSTR_CHECK)
		{
			form.contype = CONSTRAINT_CHECK;
			expression = schema_expression_bind_domain(view, constraint->raw_expr, name, type->typbasetype,
													   type->typtypmod, type->typcollation, true);
		}
		else if (constraint->contype == CONSTR_NOTNULL)
			form.contype = CONSTRAINT_NOTNULL;
		else
			elog(ERROR, "unsupported canonical domain constraint %s", names[i]);
		tuple = provider_fixed(rows->relations[cslot], &form, sizeof(form), Anum_pg_constraint_conperiod);
		if (expression)
			tuple = provider_set(rows->relations[cslot], tuple, Anum_pg_constraint_conbin, CStringGetTextDatum(nodeToString(expression)));
		rows->rows[cslot] = lappend(rows->rows[cslot], tuple);
		provider_dependency(rows, ConstraintRelationId, form.oid, 0, TypeRelationId, oid, 0, DEPENDENCY_AUTO);
		if (expression)
			provider_expression_dependencies(rows, ConstraintRelationId, form.oid, expression);
	}
}
