/*
 * Portable user-defined types: enums, domains and composite types. A type's
 * pg_type values are encoded column by column, naming every object they
 * refer to; its array type is derived again from it, and only its name is
 * kept. Enum labels are kept in sort order, a composite type's attributes by
 * name and type, and a domain's default and constraints as SQL.
 */
static char *
type_collation_name(Oid collation)
{
	HeapTuple	tuple;
	char	   *result;

	if (!OidIsValid(collation))
		return NULL;
	tuple = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for collation %u", collation);
	if (((Form_pg_collation) GETSTRUCT(tuple))->collnamespace != PG_CATALOG_NAMESPACE)
		unsupported("versioned type with a user-defined collation");
	result = dg_qualified_name("pg_catalog", NameStr(((Form_pg_collation) GETSTRUCT(tuple))->collname));
	ReleaseSysCache(tuple);
	return result;
}

typedef struct TypeLabel
{
	float4		order;
	char	   *label;
} TypeLabel;

static int
type_label_compare(const ListCell *a, const ListCell *b)
{
	float4		x = ((TypeLabel *) lfirst(a))->order;
	float4		y = ((TypeLabel *) lfirst(b))->order;

	return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * An enum's labels in sort order. Their sort order values are not kept: the
 * provider numbers them again, so the encoding is independent of history.
 * (A plain scan sees the labels a provisional view has staged.)
 */
static void
type_encode_labels(StringInfo json, Oid type)
{
	Relation	catalog = table_open(EnumRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tuple;
	List	   *labels = NIL;

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_enum form = (Form_pg_enum) GETSTRUCT(tuple);
		TypeLabel  *label;

		if (form->enumtypid != type)
			continue;
		label = palloc(sizeof(TypeLabel));
		label->order = form->enumsortorder;
		label->label = pstrdup(NameStr(form->enumlabel));
		labels = lappend(labels, label);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	list_sort(labels, type_label_compare);
	appendStringInfoString(json, ",\"labels\":[");
	foreach_ptr(TypeLabel, label, labels)
	{
		if (foreach_current_index(label))
			appendStringInfoChar(json, ',');
		escape_json(json, label->label);
	}
	appendStringInfoChar(json, ']');
}

static void
type_encode_attributes(StringInfo json, Oid relation)
{
	Relation	rel = relation_open(relation, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	bool		first = true;

	appendStringInfoString(json, ",\"attributes\":[");
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		char	   *collation;

		if (attr->attisdropped)
			continue;
		if (!first)
			appendStringInfoChar(json, ',');
		appendStringInfoChar(json, '{');
		json_field(json, "name", NameStr(attr->attname), false);
		appendStringInfoChar(json, ',');
		json_field(json, "type", routine_type_name(attr->atttypid), false);
		appendStringInfo(json, ",\"typmod\":%d", attr->atttypmod);
		collation = attr->attcollation != get_typcollation(attr->atttypid) ? type_collation_name(attr->attcollation) : NULL;
		if (collation)
		{
			appendStringInfoChar(json, ',');
			json_field(json, "collation", collation, false);
		}
		appendStringInfoChar(json, '}');
		first = false;
	}
	appendStringInfoChar(json, ']');
	relation_close(rel, AccessShareLock);
}

static int
type_entry_compare(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

/* A domain's constraints, as [name, definition, validated] in name order. */
static void
type_encode_constraints(StringInfo json, Oid type)
{
	Relation	catalog = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *entries = NIL;
	int			nest = NewGUCNestLevel();

	ScanKeyInit(&key, Anum_pg_constraint_contypid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(type));
	scan = systable_beginscan(catalog, ConstraintTypidIndexId, true, NULL, 1, &key);
	PG_TRY();
	{
		schema_expression_gucs();
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
			StringInfoData entry;
			bool		isnull;
			Datum		bin = heap_getattr(tuple, Anum_pg_constraint_conbin, RelationGetDescr(catalog), &isnull);

			if (con->contype == CONSTRAINT_CHECK && !isnull &&
				expression_calls_native(stringToNode(TextDatumGetCString(bin)), NULL))
				unsupported("versioned domain constraint calling an unversioned function");
			initStringInfo(&entry);
			appendStringInfoChar(&entry, '[');
			escape_json(&entry, NameStr(con->conname));
			appendStringInfoChar(&entry, ',');
			escape_json(&entry, TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef, ObjectIdGetDatum(con->oid))));
			appendStringInfo(&entry, ",%s]", con->convalidated ? "true" : "false");
			entries = lappend(entries, entry.data);
		}
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	list_sort(entries, type_entry_compare);
	appendStringInfoString(json, ",\"constraints\":[");
	foreach_ptr(char, entry, entries)
		appendStringInfo(json, "%s%s", foreach_current_index(entry) ? "," : "", entry);
	appendStringInfoChar(json, ']');
}

static char *
type_encode(HeapTuple tuple, TupleDesc desc)
{
	Form_pg_type type = (Form_pg_type) GETSTRUCT(tuple);
	StringInfoData json;
	char		kind[2] = {type->typtype, '\0'};

	if (type->typtype != TYPTYPE_ENUM && type->typtype != TYPTYPE_DOMAIN && type->typtype != TYPTYPE_COMPOSITE)
		unsupported("versioned base, range or pseudo types");
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGTYPE1\",");
	json_field(&json, "name", dg_qualified_name(get_namespace_name(type->typnamespace), NameStr(type->typname)), false);
	appendStringInfoChar(&json, ',');
	json_field(&json, "kind", kind, false);
	if (OidIsValid(type->typarray))
	{
		HeapTuple	array = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type->typarray));

		if (!HeapTupleIsValid(array))
			elog(ERROR, "cache lookup failed for type %u", type->typarray);
		appendStringInfoChar(&json, ',');
		json_field(&json, "array", NameStr(((Form_pg_type) GETSTRUCT(array))->typname), false);
		ReleaseSysCache(array);
	}
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		bool		isnull;
		Datum		value = heap_getattr(tuple, i + 1, desc, &isnull);
		char	   *encoded = NULL;
		Oid			output;
		bool		varlena;

		switch (i + 1)
		{
				/* Identities, derived values and privileges are not encoded. */
			case Anum_pg_type_oid:
			case Anum_pg_type_typname:
			case Anum_pg_type_typrelid:
			case Anum_pg_type_typarray:
			case Anum_pg_type_typacl:
			case Anum_pg_type_typdefault:
				continue;
			case Anum_pg_type_typnamespace:
				encoded = get_namespace_name(DatumGetObjectId(value));
				break;
			case Anum_pg_type_typowner:
				encoded = GetUserNameFromId(DatumGetObjectId(value), false);
				break;
			case Anum_pg_type_typelem:
			case Anum_pg_type_typbasetype:
				encoded = OidIsValid(DatumGetObjectId(value)) ? routine_type_name(DatumGetObjectId(value)) : NULL;
				break;
			case Anum_pg_type_typcollation:
				encoded = type_collation_name(DatumGetObjectId(value));
				break;
			case Anum_pg_type_typdefaultbin:
				if (!isnull)
				{
					int			nest = NewGUCNestLevel();
					Node	   *expression = stringToNode(TextDatumGetCString(value));

					if (expression_calls_native(expression, NULL))
						unsupported("versioned domain default calling an unversioned function");
					PG_TRY();
					{
						schema_expression_gucs();
						encoded = deparse_expression(expression, NIL, false, false);
					}
					PG_FINALLY();
					{
						AtEOXact_GUC(true, nest);
					}
					PG_END_TRY();
				}
				break;
			default:
				if (!isnull)
				{
					getTypeOutputInfo(attr->atttypid, &output, &varlena);
					encoded = OidOutputFunctionCall(output, value);
				}
				break;
		}
		appendStringInfoChar(&json, ',');
		escape_json(&json, NameStr(attr->attname));
		appendStringInfoChar(&json, ':');
		if (encoded)
			escape_json(&json, encoded);
		else
			appendStringInfoString(&json, "null");
	}
	if (type->typtype == TYPTYPE_ENUM)
		type_encode_labels(&json, type->oid);
	else if (type->typtype == TYPTYPE_COMPOSITE)
		type_encode_attributes(&json, type->typrelid);
	else
		type_encode_constraints(&json, type->oid);
	appendStringInfoChar(&json, '}');
	return json.data;
}
