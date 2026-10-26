/*
 * Rebuild a versioned view's catalog rows: its relation, columns, row type
 * and rewrite rule, whose action is the view's SQL until first read, and the
 * dependencies its definition names.
 */
static Oid
provider_view_dependency(CatalogView *view, const char *kind, const char *name)
{
	Oid			classid = !strcmp(kind, "table") ? RelationRelationId :
		!strcmp(kind, "routine") ? ProcedureRelationId : TypeRelationId;
	Oid			oid;

	if (classid == TypeRelationId)
		return routine_type_oid(provider_current_rows, view, name);

	/* Routines are built after views; binding a name gives the same handle. */
	if (classid == ProcedureRelationId)
		return CatalogViewBindObject(view, ProcedureRelationId, name, InvalidOid);
	oid = CatalogViewFindObject(view, classid, name);
	if (OidIsValid(oid) || classid != RelationRelationId)
		return oid;

	/* A native relation, which is physical. */
	{
		CatalogView *previous = CatalogViewSwitch(NULL);
		List	   *parts = stringToQualifiedNameList(name, NULL);

		oid = get_relname_relid(strVal(lsecond(parts)), get_namespace_oid(strVal(linitial(parts)), true));
		CatalogViewSwitch(previous);
	}
	return oid;
}

static void
provider_view_member(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	FormData_pg_class form = {0};
	FormData_pg_type type = {0};
	FormData_pg_type array = {0};
	FormData_pg_rewrite rule = {0};
	int			slot = provider_catalog_index(RelationRelationId);
	int			rslot = provider_catalog_index(RewriteRelationId);
	HeapTuple	tuple;
	char	   *arrayname;
	uint64		ncolumns;
	char	  **names,
			  **types,
			  **collations;
	int		   *mods;

	form.oid = CatalogViewBindObject(view, RelationRelationId, name, InvalidOid);
	form.relnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	namestrcpy(&form.relname, strVal(lsecond(parts)));
	form.relowner = get_role_oid(json_value(definition, "owner"), false);
	form.reltype = CatalogViewBindObject(view, TypeRelationId, name, InvalidOid);
	form.relkind = RELKIND_VIEW;
	form.relpersistence = RELPERSISTENCE_PERMANENT;
	form.reltuples = -1;
	form.relispopulated = true;
	form.relreplident = REPLICA_IDENTITY_NOTHING;
	form.relhasrules = true;
	run("SELECT v->>'name',v->>'type',(v->>'typmod')::int,v->>'collation' FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	ncolumns = SPI_processed;
	names = palloc_array(char *, Max(ncolumns, 1));
	types = palloc_array(char *, Max(ncolumns, 1));
	collations = palloc_array(char *, Max(ncolumns, 1));
	mods = palloc_array(int, Max(ncolumns, 1));
	for (uint64 i = 0; i < ncolumns; i++)
	{
		names[i] = strcell(i, 1);
		types[i] = strcell(i, 2);
		mods[i] = DatumGetInt32(cell(i, 3));
		collations[i] = strcell(i, 4);
	}
	form.relnatts = ncolumns;
	tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_class_relminmxid);
	run("SELECT ARRAY(SELECT jsonb_array_elements_text($1::jsonb->'options'))", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	{
		bool		isnull;
		Datum		options = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

		if (!isnull && ArrayGetNItems(ARR_NDIM(DatumGetArrayTypeP(options)), ARR_DIMS(DatumGetArrayTypeP(options))) > 0)
			tuple = provider_set(rows->relations[slot], tuple, Anum_pg_class_reloptions, options);
	}
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
	provider_dependency(rows, RelationRelationId, form.oid, 0, NamespaceRelationId, form.relnamespace, 0, DEPENDENCY_NORMAL);
	for (uint64 i = 0; i < ncolumns; i++)
	{
		TupleDesc	one = CreateTemplateTupleDesc(1);
		Oid			atttype = routine_type_oid(rows, view, types[i]);
		Form_pg_attribute attr;

		provider_init_entry(one, 1, names[i], atttype, mods[i]);
		attr = TupleDescAttr(one, 0);
		attr->attrelid = form.oid;
		attr->attnum = i + 1;
		if (collations[i])
			attr->attcollation = provider_collation(collations[i]);
		provider_add(rows, AttributeRelationId, attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
		provider_dependency(rows, RelationRelationId, form.oid, i + 1, TypeRelationId, atttype, 0, DEPENDENCY_NORMAL);
	}

	/* The view's row type and its array type, as for a table. */
	arrayname = makeObjectName("", NameStr(form.relname), NULL);
	array.oid = CatalogViewBindObject(view, TypeRelationId, dg_qualified_name(strVal(linitial(parts)), arrayname), InvalidOid);
	type.oid = form.reltype;
	type.typname = form.relname;
	type.typnamespace = form.relnamespace;
	type.typowner = form.relowner;
	type.typlen = -1;
	type.typtype = TYPTYPE_COMPOSITE;
	type.typcategory = TYPCATEGORY_COMPOSITE;
	type.typisdefined = true;
	type.typdelim = ',';
	type.typrelid = form.oid;
	type.typarray = array.oid;
	type.typinput = F_RECORD_IN;
	type.typoutput = F_RECORD_OUT;
	type.typreceive = F_RECORD_RECV;
	type.typsend = F_RECORD_SEND;
	type.typalign = TYPALIGN_DOUBLE;
	type.typstorage = TYPSTORAGE_EXTENDED;
	type.typtypmod = -1;
	provider_add(rows, TypeRelationId, &type, sizeof(type), Anum_pg_type_typcollation);
	provider_dependency(rows, TypeRelationId, type.oid, 0, RelationRelationId, form.oid, 0, DEPENDENCY_INTERNAL);
	provider_type_array(rows, &type, arrayname);

	/* Its rule: the view's SQL, analyzed when the rule is first read. */
	rule.oid = CatalogViewBindObject(view, RewriteRelationId, psprintf("%s/%s", name, ViewSelectRuleName), InvalidOid);
	namestrcpy(&rule.rulename, ViewSelectRuleName);
	rule.ev_class = form.oid;
	rule.ev_type = '0' + CMD_SELECT;
	rule.ev_enabled = RULE_FIRES_ON_ORIGIN;
	rule.is_instead = true;
	tuple = provider_fixed(rows->relations[rslot], &rule, sizeof(rule), Anum_pg_rewrite_is_instead);
	tuple = provider_set(rows->relations[rslot], tuple, Anum_pg_rewrite_ev_qual, CStringGetTextDatum("<>"));
	tuple = provider_set(rows->relations[rslot], tuple, Anum_pg_rewrite_ev_action,
						 CStringGetTextDatum(nodeToString(list_make1(makeString(json_value(definition, "query"))))));
	rows->rows[rslot] = lappend(rows->rows[rslot], tuple);
	provider_dependency(rows, RewriteRelationId, rule.oid, 0, RelationRelationId, form.oid, 0, DEPENDENCY_INTERNAL);
	run("SELECT v->>0,v->>1,v->>2 FROM jsonb_array_elements($1::jsonb->'dependencies') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	{
		uint64		n = SPI_processed;
		char	  **kinds = palloc_array(char *, Max(n, 1));
		char	  **targets = palloc_array(char *, Max(n, 1));
		char	  **columns = palloc_array(char *, Max(n, 1));

		for (uint64 i = 0; i < n; i++)
		{
			kinds[i] = strcell(i, 1);
			targets[i] = strcell(i, 2);
			columns[i] = strcell(i, 3);
		}
		for (uint64 i = 0; i < n; i++)
		{
			Oid			target = provider_view_dependency(view, kinds[i], targets[i]);
			int			column = 0;

			if (!OidIsValid(target))
				elog(ERROR, "versioned view %s depends on missing %s %s", name, kinds[i], targets[i]);
			if (columns[i])
			{
				/* A versioned table's columns are numbered as its definition lists them. */
				bool		versioned = false;

				for (int j = 0; j < provider_definition_count; j++)
				{
					if (strcmp(json_value(provider_definition_list[j], "name"), targets[i]))
						continue;
					if (!strcmp(provider_definition_kinds[j], "table"))
						column = provider_attribute(provider_columns(provider_definition_list[j]), columns[i]);
					else if (!strcmp(provider_definition_kinds[j], "view"))
					{
						run("SELECT ord::int FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) WHERE v->>'name'=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(provider_definition_list[j]), CStringGetTextDatum(columns[i])), NULL, true);
						if (SPI_processed)
							column = DatumGetInt32(cell(0, 1));
					}
					else
						continue;
					versioned = true;
				}
				if (!versioned)
				{
					/* A native relation's column, which is physical. */
					CatalogView *previous = CatalogViewSwitch(NULL);

					column = get_attnum(target, columns[i]);
					CatalogViewSwitch(previous);
				}
			}
			provider_dependency(rows, RewriteRelationId, rule.oid, 0,
								!strcmp(kinds[i], "table") ? RelationRelationId : !strcmp(kinds[i], "routine") ? ProcedureRelationId : TypeRelationId,
								target, column, DEPENDENCY_NORMAL);
		}
	}
}
