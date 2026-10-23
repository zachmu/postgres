/* Native ALTER runs against private command-visible catalog tuples. Publication
 * serializes the final definitions once; intermediate roots are not published. */
static bool
native_metadata_action(AlterTableType type)
{
	return type == AT_SetStatistics || type == AT_SetOptions || type == AT_ResetOptions ||
		type == AT_SetStorage || type == AT_SetCompression || type == AT_SetNotNull || type == AT_DropNotNull;
}

static char *
mutation_columns(CatalogView *view, Oid relation, const char *definition)
{
	Relation	catalog = table_open(AttributeRelationId, AccessShareLock);
	ListCell   *lc;
	char	   *result = pstrdup(definition);

	foreach(lc, CatalogViewTuples(view, AttributeRelationId))
	{
		HeapTuple	tuple = lfirst(lc);
		Form_pg_attribute attr = (Form_pg_attribute) GETSTRUCT(tuple);
		bool		statsnull,
					optionsnull;
		Datum		stats,
					options;
		char		nulls[8] = "       ";
		char		storage[2] = {attr->attstorage, 0};
		char		compression[2] = {attr->attcompression, 0};

		if (attr->attrelid != relation || attr->attnum <= 0)
			continue;
		stats = heap_getattr(tuple, Anum_pg_attribute_attstattarget, RelationGetDescr(catalog), &statsnull);
		options = heap_getattr(tuple, Anum_pg_attribute_attoptions, RelationGetDescr(catalog), &optionsnull);
		nulls[2] = statsnull ? 'n' : ' ';
		nulls[3] = optionsnull ? 'n' : ' ';
		nulls[4] = attr->attstorage == get_typstorage(attr->atttypid) ? 'n' : ' ';
		nulls[5] = attr->attcompression == '\0' ? 'n' : ' ';
		run("SELECT jsonb_set($1::jsonb,ARRAY['columns',$2::text],(($1::jsonb->'columns'->$2)-'statistics'-'options'-'storage'-'compression') || jsonb_strip_nulls(jsonb_build_object('notnull',$7::boolean,'statistics',$3::int,'options',(SELECT jsonb_agg(v ORDER BY v COLLATE \"C\") FROM unnest($4::text[]) v),'storage',$5::text,'compression',$6::text)))::text", 7, VC_TYPES(TEXTOID, INT4OID, INT4OID, TEXTARRAYOID, TEXTOID, TEXTOID, BOOLOID), VC_ARGS(CStringGetTextDatum(result), Int32GetDatum(attr->attnum - 1), stats, options, CStringGetTextDatum(storage), CStringGetTextDatum(compression), BoolGetDatum(attr->attnotnull)), nulls, true);
		result = strcell(0, 1);
	}
	table_close(catalog, AccessShareLock);
	return result;
}

static CatalogView *native_mutation_view;

/* Native validation scans borrow the unchanged working-set data while catalog
 * definitions remain private. No provisional handle is a physical resource. */
DGState *
dg_catalog_mutation_load(Relation definition, Snapshot snapshot)
{
	CatalogView *view = CatalogViewObjectContext(RelationGetRelid(definition));
	CatalogView *previous;
	Relation	source;
	DGState    *state = NULL;
	const char *name;

	if (view != native_mutation_view || !CatalogViewIsProvisional(view))
		elog(ERROR, "native catalog validation has no storage authority");
	name = CatalogViewObjectIdentity(RelationGetRelid(definition), RelationRelationId);
	previous = CatalogViewSwitch(NULL);
	PG_TRY();
	{
		source = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
		state = dg_load(source, snapshot);
		dg_storage_close_relation(source);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return state;
}

static char *
mutation_constraints(CatalogView *view, Oid relation, const char *definition)
{
	ListCell   *lc;
	StringInfoData json;
	CatalogView *previous = CatalogViewSwitch(view);
	bool		first = true;

	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	PG_TRY();
	{
		foreach(lc, CatalogViewTuples(view, ConstraintRelationId))
		{
			HeapTuple	tuple = lfirst(lc);
			Form_pg_constraint constraint = (Form_pg_constraint) GETSTRUCT(tuple);
			char	   *sql;

			if (constraint->conrelid != relation)
				continue;
			if (constraint->contype != CONSTRAINT_NOTNULL && constraint->contype != CONSTRAINT_PRIMARY && constraint->contype != CONSTRAINT_UNIQUE)
				unsupported("canonical codec for native constraint type");
			sql = TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef, ObjectIdGetDatum(constraint->oid)));
			if (!first)
				appendStringInfoChar(&json, ',');
			first = false;
			appendStringInfoChar(&json, '[');
			escape_json(&json, NameStr(constraint->conname));
			appendStringInfoChar(&json, ',');
			escape_json(&json, sql);
			appendStringInfoChar(&json, ']');
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	appendStringInfoChar(&json, ']');
	run("SELECT jsonb_set($1::jsonb,'{constraints}',coalesce((SELECT jsonb_agg(v ORDER BY (v->>0) COLLATE \"C\") FROM jsonb_array_elements($2::jsonb) v),'[]'::jsonb))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(json.data)), NULL, true);
	return strcell(0, 1);
}

static void
native_metadata_publish(Capture current, VersionRef working, const char *name, List *commands)
{
	CatalogView *view = CatalogViewCreate("provisional native ALTER");
	CatalogView *previous;
	CatalogView *saved_mutation = native_mutation_view;
	bytea	  **members;
	char	  **kinds,
			  **names,
			  **payloads;
	Datum	   *children;
	StringInfoData json;
	int			count;

	provider_populate(view, current.hash);
	CatalogViewBeginMutation(view);
	native_mutation_view = view;
	previous = CatalogViewSwitch(view);
	PG_TRY();
	{
		Oid			id = CatalogViewLookupObject(view, RelationRelationId, name);

		AlterTableInternal(id, copyObject(commands), false);
		CommandCounterIncrement();
		CatalogViewSwitch(NULL);
		run("SELECT t.objhash,t.objkind,CASE WHEN t.objkind IN ('table','index') THEN convert_from(t.objpayload,'UTF8')::jsonb->>'name' END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
		count = SPI_processed;
		members = palloc_array(bytea *, Max(count, 1));
		kinds = palloc_array(char *, Max(count, 1));
		names = palloc_array(char *, Max(count, 1));
		payloads = palloc_array(char *, Max(count, 1));
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
			payloads[i] = metadata(members[i], kinds[i]);
			if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
			{
				bytea	   *oldschema = child(members[i], 1);
				char	   *old = metadata(oldschema, "schema");
				Oid			object = CatalogViewLookupObject(view, RelationRelationId, names[i]);
				char	   *definition = mutation_columns(view, object, old);

				if (!strcmp(kinds[i], "table"))
					definition = mutation_constraints(view, object, definition);

				if (strcmp(definition, old))
				{
					bytea	   *schema = put("schema", definition, NULL, 0);
					bytea	   *content = child(members[i], 2);
					Oid			resource = binding_relation(dg_current_branch(), names[i], oldschema);

					if (!OidIsValid(resource))
						elog(ERROR, "native mutation storage binding is unavailable");
					run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payloads[i]), CStringGetTextDatum(definition)), NULL, true);
					payloads[i] = strcell(0, 1);
					members[i] = put(kinds[i], payloads[i], VC_ARGS(PointerGetDatum(schema), PointerGetDatum(content)), 2);
					binding_record(dg_current_branch(), names[i], schema, resource);
				}
			}
			if (i)
				appendStringInfoChar(&json, ',');
			if (!strcmp(kinds[i], "namespace"))
				appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payloads[i]);
			else
				appendStringInfoString(&json, payloads[i]);
			children[i] = PointerGetDatum(members[i]);
		}
		appendStringInfoChar(&json, ']');
		run("SELECT jsonb_set($1::jsonb,'{members}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(json.data)), NULL, true);
		{
			bytea	   *database = canonical_database(strcell(0, 1), children, count);

			(void) provider_view(database);
			move_ref(branch_ref("working"), working, database);
			advance_snapshot();
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		native_mutation_view = saved_mutation;
		CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}
