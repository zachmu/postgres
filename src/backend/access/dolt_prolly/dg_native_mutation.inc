/* Native ALTER runs against private command-visible catalog tuples. Publication
 * serializes the final definitions once; intermediate roots are not published. */
static bool
native_alter_action(AlterTableType type)
{
	return type == AT_SetStatistics || type == AT_SetOptions || type == AT_ResetOptions ||
		type == AT_SetStorage || type == AT_SetCompression || type == AT_SetNotNull || type == AT_DropNotNull || type == AT_AddColumn || type == AT_DropColumn;
}

static char *
mutation_columns(CatalogView *view, Oid relation, const char *definition, int **mapping, int *count)
{
	Relation	catalog = table_open(AttributeRelationId, AccessShareLock);
	ListCell   *lc;
	StringInfoData columns;
	HeapTuple	attributes[MaxHeapAttributeNumber] = {0};

	*mapping = palloc_array(int, MaxHeapAttributeNumber);
	*count = 0;
	initStringInfo(&columns);
	appendStringInfoChar(&columns, '[');
	foreach(lc, CatalogViewTuples(view, AttributeRelationId))
	{
		HeapTuple	tuple = lfirst(lc);
		Form_pg_attribute attr = (Form_pg_attribute) GETSTRUCT(tuple);

		if (attr->attrelid == relation && attr->attnum > 0)
			attributes[attr->attnum - 1] = tuple;
	}

	for (int ordinal = 0; ordinal < MaxHeapAttributeNumber; ordinal++)
	{
		HeapTuple	tuple = attributes[ordinal];
		Form_pg_attribute attr;
		char	   *column;

		bool		statsnull,
					optionsnull;
		Datum		stats,
					options;
		char		nulls[8] = "       ";
		char		storage[2] = {0};
		char		compression[2] = {0};

		if (!tuple)
			continue;
		attr = (Form_pg_attribute) GETSTRUCT(tuple);
		storage[0] = attr->attstorage;
		compression[0] = attr->attcompression;

		if (attr->attisdropped)
			continue;
		if (attr->atthasdef || attr->attidentity || attr->attgenerated || attr->atthasmissing)
			unsupported("canonical native column expression codec");
		run("SELECT coalesce($1::jsonb->'columns'->$2, jsonb_build_object('name',$3::text,'comment',NULL,'type',t.typname,'typmod',$5::int,'notnull',false,'collation',CASE WHEN $6::oid=0 THEN NULL ELSE (SELECT jsonb_build_array(n.nspname,c.collname,c.collprovider,c.collisdeterministic,c.collcollate,c.collctype,c.colllocale,c.collversion) FROM pg_collation c JOIN pg_namespace n ON n.oid=c.collnamespace WHERE c.oid=$6) END))::text FROM pg_type t WHERE t.oid=$4 AND t.typnamespace='pg_catalog'::regnamespace", 6, VC_TYPES(TEXTOID, INT4OID, TEXTOID, OIDOID, INT4OID, OIDOID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(ordinal), CStringGetTextDatum(NameStr(attr->attname)), ObjectIdGetDatum(attr->atttypid), Int32GetDatum(attr->atttypmod), ObjectIdGetDatum(attr->attcollation)), NULL, true);
		if (SPI_processed != 1)
			unsupported("canonical native column user-defined type codec");
		column = strcell(0, 1);
		stats = heap_getattr(tuple, Anum_pg_attribute_attstattarget, RelationGetDescr(catalog), &statsnull);
		options = heap_getattr(tuple, Anum_pg_attribute_attoptions, RelationGetDescr(catalog), &optionsnull);
		nulls[2] = statsnull ? 'n' : ' ';
		nulls[3] = optionsnull ? 'n' : ' ';
		nulls[4] = attr->attstorage == get_typstorage(attr->atttypid) ? 'n' : ' ';
		nulls[5] = attr->attcompression == '\0' ? 'n' : ' ';
		run("SELECT (($1::jsonb-'statistics'-'options'-'storage'-'compression') || jsonb_strip_nulls(jsonb_build_object('notnull',$7::boolean,'statistics',$3::int,'options',(SELECT jsonb_agg(v ORDER BY v COLLATE \"C\") FROM unnest($4::text[]) v),'storage',$5::text,'compression',$6::text)))::text", 7, VC_TYPES(TEXTOID, INT4OID, INT4OID, TEXTARRAYOID, TEXTOID, TEXTOID, BOOLOID), VC_ARGS(CStringGetTextDatum(column), Int32GetDatum(attr->attnum - 1), stats, options, CStringGetTextDatum(storage), CStringGetTextDatum(compression), BoolGetDatum(attr->attnotnull)), nulls, true);
		if (*count)
			appendStringInfoChar(&columns, ',');
		appendStringInfoString(&columns, strcell(0, 1));
		(*mapping)[(*count)++] = ordinal;
	}
	table_close(catalog, AccessShareLock);
	appendStringInfoChar(&columns, ']');
	run("SELECT jsonb_set($1::jsonb,'{columns}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(columns.data)), NULL, true);
	return strcell(0, 1);
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
		{
			TupleDesc	desc = RelationGetDescr(definition);
			int			oldcount = RelationGetDescr(source)->natts;
			int		   *mapping = palloc_array(int, Max(desc->natts, 1));
			bool		changed = desc->natts != oldcount;

			for (int i = 0; i < desc->natts; i++)
			{
				bool		dropped = TupleDescAttr(desc, i)->attisdropped;

				mapping[i] = dropped ? oldcount : i;
				changed |= dropped;
			}
			if (changed)
			{
				state->content = alter_rows(state, oldcount, desc->natts, mapping);
				dg_history_handles(state);
			}
			pfree(mapping);
		}
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
native_alter_publish(Capture current, VersionRef working, const char *name, List *commands, DGState *state, int oldcount)
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
	bool		rewrite = false;
	PLTree		tree = state->content;

	provider_populate(view, current.hash);
	CatalogViewBeginMutation(view);
	native_mutation_view = view;
	previous = CatalogViewSwitch(view);
	PG_TRY();
	{
		Oid			id = CatalogViewLookupObject(view, RelationRelationId, name);

		ListCell   *lc;

		foreach(lc, commands)
		{
			AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

			if (action->subtype == AT_AddColumn)
			{
				ColumnDef  *col = castNode(ColumnDef, action->def);

				/*
				 * PostgreSQL ignores an existing IF NOT EXISTS definition
				 * before transforming defaults or subsidiary constraints.
				 */
				if (action->missing_ok && get_attnum(id, col->colname) != InvalidAttrNumber)
					continue;
				if (col->constraints || col->raw_default || col->cooked_default || col->identity || col->generated)
					unsupported("native ADD COLUMN expression and constraint codecs");
			}
		}

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
				int		   *mapping;
				int			attributes;
				char	   *definition = mutation_columns(view, object, old, &mapping, &attributes);

				if (!strcmp(names[i], name))
				{
					rewrite = attributes != oldcount;
					for (int j = 0; j < attributes; j++)
						if (mapping[j] != j)
							rewrite = true;
					if (rewrite)
						tree = alter_rows(state, oldcount, attributes, mapping);
				}

				if (!strcmp(kinds[i], "table"))
					definition = mutation_constraints(view, object, definition);

				if (strcmp(definition, old))
				{
					bytea	   *schema = put("schema", definition, NULL, 0);
					bytea	   *content = rewrite && !strcmp(names[i], name) ? dg_history_export_tree(&tree) : child(members[i], 2);
					Oid			resource = binding_relation(dg_current_branch(), names[i], oldschema);

					if (!OidIsValid(resource))
						elog(ERROR, "native mutation storage binding is unavailable");
					run("SELECT jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex')))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payloads[i]), CStringGetTextDatum(definition)), NULL, true);
					payloads[i] = strcell(0, 1);
					if (rewrite && !strcmp(names[i], name))
					{
						char		root[PL_HASH_SIZE * 2 + 1];

						hex_encode((char *) tree.root.bytes, PL_HASH_SIZE, root);
						root[PL_HASH_SIZE * 2] = 0;
						run("SELECT jsonb_set($1::jsonb,'{content}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payloads[i]), CStringGetTextDatum(root)), NULL, true);
						payloads[i] = strcell(0, 1);
					}
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
			if (rewrite)
			{
				Relation	target = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);

				dg_history_install(target, state, tree);
				dg_storage_close_relation(target);
				UpdateActiveSnapshotCommandId();
				move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
			}
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
