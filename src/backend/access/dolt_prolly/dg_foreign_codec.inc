/* Persist symbolic operator signatures and index identity, never runtime OIDs. */
static char *
foreign_metadata(HeapTuple tuple)
{
	Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
	StringInfoData json;
	const int	fields[] = {Anum_pg_constraint_conpfeqop, Anum_pg_constraint_conppeqop, Anum_pg_constraint_conffeqop};
	const char *names[] = {"pf", "pp", "ff"};

	if (con->conparentid || con->conperiod || !con->conenforced)
		unsupported("partitioned, temporal or unenforced foreign key codec");
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"index\":");
	escape_json(&json, quote_qualified_identifier(get_namespace_name(get_rel_namespace(con->conindid)), get_rel_name(con->conindid)));
	for (int i = 0; i < lengthof(fields); i++)
	{
		bool		isnull;
		Datum		value = SysCacheGetAttr(CONSTROID, tuple, fields[i], &isnull);
		Datum	   *operators;
		bool	   *nulls;
		int			count;

		if (isnull)
			elog(ERROR, "missing foreign key operators");
		deconstruct_array(DatumGetArrayTypeP(value), OIDOID, sizeof(Oid), true, TYPALIGN_INT, &operators, &nulls, &count);
		appendStringInfo(&json, ",\"%s\":[", names[i]);
		for (int j = 0; j < count; j++)
		{
			if (nulls[j])
				elog(ERROR, "null foreign key operator");
			if (j)
				appendStringInfoChar(&json, ',');
			escape_json(&json, format_operator_qualified(DatumGetObjectId(operators[j])));
		}
		appendStringInfoChar(&json, ']');
	}
	appendStringInfoChar(&json, '}');
	return json.data;
}

static char *
foreign_capture_definition(const char *definition, Oid relation)
{
	Oid		   *ids;
	int			count;
	char	   *result = pstrdup(definition);

	run("SELECT oid FROM pg_constraint WHERE conrelid=$1 AND contype='f'", 1, VC_TYPES(OIDOID), VC_ARGS(ObjectIdGetDatum(relation)), NULL, true);
	count = SPI_processed;
	ids = palloc_array(Oid, Max(count, 1));
	for (int i = 0; i < count; i++)
		ids[i] = DatumGetObjectId(cell(i, 1));
	for (int i = 0; i < count; i++)
	{
		HeapTuple	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(ids[i]));
		Form_pg_constraint con;
		char	   *metadata;

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "foreign key disappeared during capture");
		con = (Form_pg_constraint) GETSTRUCT(tuple);
		metadata = foreign_metadata(tuple);
		run("SELECT jsonb_set($1::jsonb,'{constraints}',(SELECT jsonb_agg(CASE WHEN v->>0=$2 THEN v || jsonb_build_array($3::jsonb) ELSE v END ORDER BY ord) FROM jsonb_array_elements($1::jsonb->'constraints') WITH ORDINALITY a(v,ord)))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(result), CStringGetTextDatum(NameStr(con->conname)), CStringGetTextDatum(metadata)), NULL, true);
		result = strcell(0, 1);
		ReleaseSysCache(tuple);
	}
	return result;
}
