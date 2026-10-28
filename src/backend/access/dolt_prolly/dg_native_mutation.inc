static List *table_index_names(const char *schema_json, const char *table);
static bool name_selected(List *names, const char *name);

/* Native ALTER runs against private command-visible catalog tuples. Publication
 * serializes the final definitions once; intermediate roots are not published. */
static bool
native_alter_action(AlterTableType type)
{
	return type == AT_AlterConstraint || type == AT_AddIndex || type == AT_SetStatistics || type == AT_SetOptions || type == AT_ResetOptions ||
		type == AT_SetStorage || type == AT_SetCompression || type == AT_SetNotNull || type == AT_DropNotNull || type == AT_AddColumn || type == AT_DropColumn || type == AT_ColumnDefault || type == AT_AddConstraint || type == AT_DropConstraint || type == AT_ValidateConstraint || type == AT_AlterColumnType ||
		type == AT_AddIdentity || type == AT_SetIdentity || type == AT_DropIdentity;
}

static CatalogView *native_mutation_view;
static DGState *native_mutation_source;
static DGState *native_mutation_destination;
static Oid	native_mutation_relation;
static int	native_mutation_width;
static bool native_mutation_rewritten;


static void
mutation_validate_tuple(Relation catalog, HeapTuple tuple)
{
	int			field;
	bool		isnull;
	Datum		expression;

	if (RelationGetRelid(catalog) == AttrDefaultRelationId)
		field = Anum_pg_attrdef_adbin;
	else if (RelationGetRelid(catalog) == ConstraintRelationId)
		field = Anum_pg_constraint_conbin;
	else
		return;
	expression = heap_getattr(tuple, field, RelationGetDescr(catalog), &isnull);
	if (!isnull)
		(void) schema_expression_check(stringToNode(TextDatumGetCString(expression)), NULL);
}

static char *
mutation_columns(CatalogView *view, Oid relation, const char *definition, int **mapping, int *count)
{
	Relation	catalog = table_open(AttributeRelationId, AccessShareLock);
	Relation	defaults = table_open(AttrDefaultRelationId, AccessShareLock);
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
		char	   *column,
				   *typename;

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
		if (attr->atthasmissing && !native_mutation_destination)
			unsupported("canonical native column expression codec");
		/* A built-in type by its name, a versioned one qualified. */
		{
			HeapTuple	type = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attr->atttypid));

			if (!HeapTupleIsValid(type))
				elog(ERROR, "cache lookup failed for type %u", attr->atttypid);
			if (((Form_pg_type) GETSTRUCT(type))->typnamespace == PG_CATALOG_NAMESPACE)
				typename = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(type))->typname));
			else
				typename = routine_type_name(attr->atttypid);
			ReleaseSysCache(type);
		}
		run("SELECT (coalesce($1::jsonb->'columns'->$2,'{}'::jsonb) || jsonb_build_object('name',$3::text,'comment',$1::jsonb->'columns'->$2->'comment','type',$4::text,'typmod',$5::int,'notnull',false,'collation',CASE WHEN $6::oid=0 THEN NULL ELSE (SELECT jsonb_build_array(n.nspname,c.collname,c.collprovider,c.collisdeterministic,c.collcollate,c.collctype,c.colllocale,c.collversion) FROM pg_collation c JOIN pg_namespace n ON n.oid=c.collnamespace WHERE c.oid=$6) END))::text", 6, VC_TYPES(TEXTOID, INT4OID, TEXTOID, TEXTOID, INT4OID, OIDOID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(ordinal), CStringGetTextDatum(NameStr(attr->attname)), CStringGetTextDatum(typename), Int32GetDatum(attr->atttypmod), ObjectIdGetDatum(attr->attcollation)), NULL, true);
		column = strcell(0, 1);
		{
			ListCell   *dc;
			char	   *sql = NULL;
			CatalogView *previous = CatalogViewSwitch(view);

			PG_TRY();
			{
				foreach(dc, CatalogViewTuples(view, AttrDefaultRelationId))
				{
					HeapTuple	def = lfirst(dc);
					Form_pg_attrdef form = (Form_pg_attrdef) GETSTRUCT(def);
					bool		isnull;
					Datum		expression;

					if (form->adrelid != relation || form->adnum != attr->attnum)
						continue;
					expression = heap_getattr(def, Anum_pg_attrdef_adbin, RelationGetDescr(defaults), &isnull);
					if (isnull)
						elog(ERROR, "null native column default expression");
					sql = schema_expression_sql(stringToNode(TextDatumGetCString(expression)), relation);
				}
			}
			PG_FINALLY();
			{
				CatalogViewSwitch(previous);
			}
			PG_END_TRY();
			if (attr->atthasdef != (sql != NULL))
				elog(ERROR, "native attribute/default catalogs disagree");
			{
				/* A generated column's expression is its default's, with a kind. */
				char		generated[2] = {attr->attgenerated, '\0'};

				run("SELECT (($1::jsonb-'default'-'generated') || jsonb_strip_nulls(jsonb_build_object('default',$2::text,'generated',$3::text)))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID),
					VC_ARGS(CStringGetTextDatum(column), sql ? CStringGetTextDatum(sql) : (Datum) 0, CStringGetTextDatum(generated)),
					sql ? (attr->attgenerated ? NULL : "  n") : (attr->attgenerated ? " n " : " nn"), true);
			}
			column = strcell(0, 1);
		}

		/* An identity column names its global sequence, never its OID. */
		if (attr->attidentity)
		{
			char		identity[2] = {attr->attidentity, '\0'};
			CatalogView *outer = CatalogViewSwitch(view);
			Relation	rel = relation_open(relation, NoLock);
			Oid			sequence = getIdentitySequence(rel, attr->attnum, false);
			char	   *name;

			relation_close(rel, NoLock);
			CatalogViewSwitch(NULL);
			name = dg_qualified_name(get_namespace_name(get_rel_namespace(sequence)), get_rel_name(sequence));
			CatalogViewSwitch(outer);
			run("SELECT ($1::jsonb || jsonb_build_object('identity',$2::text,'sequence',$3::text))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID),
				VC_ARGS(CStringGetTextDatum(column), CStringGetTextDatum(identity), CStringGetTextDatum(name)), NULL, true);
			column = strcell(0, 1);
		}
		else
		{
			run("SELECT ($1::jsonb-'identity'-'sequence')::text", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(column)), NULL, true);
			column = strcell(0, 1);
		}
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
	table_close(defaults, AccessShareLock);
	appendStringInfoChar(&columns, ']');
	run("SELECT jsonb_set($1::jsonb,'{columns}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(columns.data)), NULL, true);
	return strcell(0, 1);
}


/* Whether a native rewrite holds a destination state in the working store. */
bool
dg_native_rewrite_active(void)
{
	return native_mutation_destination != NULL;
}

bool
dg_catalog_begin_rewrite(Relation definition)
{
	MemoryContext old;

	if (!CatalogViewIsProvisional(definition->rd_catalogview))
		return false;
	if (definition->rd_catalogview != native_mutation_view || RelationGetRelid(definition) != native_mutation_relation || native_mutation_destination)
		elog(ERROR, "invalid native catalog rewrite authority");
	old = MemoryContextSwitchTo(GetMemoryChunkContext(native_mutation_source));
	native_mutation_destination = palloc0(sizeof(DGState));
	native_mutation_destination->store = native_mutation_source->store;
	native_mutation_destination->nextid = 1;
	native_mutation_width = RelationGetDescr(definition)->natts;
	dg_check(pl_build(native_mutation_source->store, NULL, 0, &native_mutation_destination->content));
	MemoryContextSwitchTo(old);
	return true;
}

void
dg_catalog_finish_rewrite(Relation definition)
{
	if (definition->rd_catalogview != native_mutation_view || RelationGetRelid(definition) != native_mutation_relation || !native_mutation_destination)
		elog(ERROR, "native rewrite completion has no storage authority");
	native_mutation_rewritten = true;
}

DGState *
dg_catalog_rewrite_state(Relation definition)
{
	if (definition->rd_catalogview != native_mutation_view || RelationGetRelid(definition) != native_mutation_relation || !native_mutation_destination)
		elog(ERROR, "native catalog rewrite has no insertion authority");
	return native_mutation_destination;
}

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
	Oid			user;
	int			security;

	if (view != native_mutation_view || !CatalogViewIsProvisional(view))
		elog(ERROR, "native catalog validation has no storage authority");
	if (native_mutation_rewritten && RelationGetRelid(definition) == native_mutation_relation)
	{
		/* A reader's copy: releasing it must not free the shared store. */
		state = palloc0(sizeof(DGState));
		*state = *native_mutation_destination;
		state->shared = true;
		memset(&state->cleanup, 0, sizeof(state->cleanup));
		return state;
	}
	name = CatalogViewObjectIdentity(RelationGetRelid(definition), RelationRelationId);
	previous = CatalogViewSwitch(NULL);
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		source = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
		state = dg_load(source, snapshot);
		dg_storage_close_relation(source);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return state;
}

/*
 * Release |state|, from dg_catalog_mutation_load, unless it is the rewrite's
 * destination, which the rewrite owns: it shares the source's store.
 */
void
dg_catalog_mutation_release(DGState *state)
{
	if (state != native_mutation_destination)
		dg_release(state);
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

			/* A constraint trigger defines its constraint. */
			if (constraint->conrelid != relation || constraint->contype == CONSTRAINT_TRIGGER)
				continue;
			if (constraint->contype != CONSTRAINT_NOTNULL && constraint->contype != CONSTRAINT_PRIMARY && constraint->contype != CONSTRAINT_UNIQUE && constraint->contype != CONSTRAINT_CHECK && constraint->contype != CONSTRAINT_FOREIGN)
				unsupported("canonical codec for native constraint type");
			if (constraint->contype == CONSTRAINT_CHECK)
			{
				bool		isnull;
				Datum		expression = SysCacheGetAttr(CONSTROID, tuple, Anum_pg_constraint_conbin, &isnull);

				if (isnull)
					elog(ERROR, "null native CHECK expression");
				(void) schema_expression_check(stringToNode(TextDatumGetCString(expression)), NULL);
			}
			{
				int			nest = NewGUCNestLevel();

				PG_TRY(deparse);
				{
					schema_expression_gucs();
					sql = TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef, ObjectIdGetDatum(constraint->oid)));
				}
				PG_FINALLY(deparse);
				{
					AtEOXact_GUC(true, nest);
				}
				PG_END_TRY(deparse);
			}
			if (!first)
				appendStringInfoChar(&json, ',');
			first = false;
			appendStringInfoChar(&json, '[');
			escape_json(&json, NameStr(constraint->conname));
			appendStringInfoChar(&json, ',');
			escape_json(&json, sql);
			if (constraint->contype == CONSTRAINT_FOREIGN)
			{
				appendStringInfoChar(&json, ',');
				appendStringInfoString(&json, foreign_metadata(tuple));
			}
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

typedef struct MutationIndex
{
	Oid			id;
	Oid			owner;
	char	   *name;
	char	   *sortname;
	char	   *sql;
	bytea	   *schema;
	bytea	   *member;
	Oid			resource;
} MutationIndex;

static bool native_mutation_indexes;

static void
mutation_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_catalogview != native_mutation_view ||
		relation->rd_rel->relkind != RELKIND_INDEX ||
		relation->rd_rel->relam != DOLT_PROLLY_INDEX_AM_OID)
		unsupported("native mutation storage effect outside prolly indexes");
	if (effect != CATALOG_STORAGE_CREATE && IndexGetRelation(RelationGetRelid(relation), false) != native_mutation_relation)
		unsupported("native mutation index belongs to another table");
	native_mutation_indexes = true;
}

static int
mutation_index_compare(const ListCell *a, const ListCell *b)
{
	return strcmp(((MutationIndex *) lfirst(a))->sortname, ((MutationIndex *) lfirst(b))->sortname);
}

/* Native pg_index is the authority after ALTER has completed all its passes. */
static List *
mutation_indexes(CatalogView *view, Oid table)
{
	List	   *result = NIL;
	ListCell   *lc;
	CatalogView *previous = CatalogViewSwitch(view);
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		schema_expression_gucs();
		foreach(lc, CatalogViewTuples(view, IndexRelationId))
		{
			HeapTuple	tuple = lfirst(lc);
			Form_pg_index index = (Form_pg_index) GETSTRUCT(tuple);
			MutationIndex *item;

			if (index->indrelid != table)
				continue;
			if (index->indisexclusion || !index->indisvalid || !index->indisready)
				unsupported("native index validity or exclusion constraint codec");
			for (int i = 0; i < index->indnatts; i++)
				if (index->indkey.values[i] <= 0)
					unsupported("canonical expression index codec");
			if (!heap_attisnull(tuple, Anum_pg_index_indpred, NULL))
				unsupported("canonical partial index codec");
			item = palloc0(sizeof(*item));
			item->id = index->indexrelid;
			{
				HeapTuple	relation = SearchSysCache1(RELOID, ObjectIdGetDatum(item->id));

				item->owner = ((Form_pg_class) GETSTRUCT(relation))->relowner;
				ReleaseSysCache(relation);
			}
			item->sortname = get_rel_name(item->id);
			item->name = dg_qualified_name(get_namespace_name(get_rel_namespace(item->id)), get_rel_name(item->id));
			item->sql = TextDatumGetCString(DirectFunctionCall1(pg_get_indexdef, ObjectIdGetDatum(item->id)));
			result = lappend(result, item);
		}
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	list_sort(result, mutation_index_compare);
	return result;
}

static char *
mutation_index_list(const char *definition, List *indexes)
{
	StringInfoData json;
	ListCell   *lc;

	initStringInfo(&json);
	appendStringInfoChar(&json, '[');
	foreach(lc, indexes)
	{
		MutationIndex *item = lfirst(lc);

		if (lc != list_head(indexes))
			appendStringInfoChar(&json, ',');
		escape_json(&json, item->sql);
	}
	appendStringInfoChar(&json, ']');
	run("SELECT jsonb_set($1::jsonb,'{indexes}',(SELECT coalesce(jsonb_agg(v ORDER BY v #>> '{}' COLLATE \"C\"),'[]'::jsonb) FROM jsonb_array_elements($2::jsonb) v))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(json.data)), NULL, true);
	return strcell(0, 1);
}

/* Whether |action| changes a column's identity, which a global sequence serves. */
static bool
identity_action(AlterTableCmd *action)
{
	return action->subtype == AT_AddIdentity || action->subtype == AT_SetIdentity ||
		action->subtype == AT_DropIdentity;
}

/*
 * As ATExecAddIdentity, ATExecSetIdentity and ATExecDropIdentity, on table
 * |relid| of the provisional view. Sequences are global, as at CREATE: one
 * is created natively for an added identity, sequence options change it
 * natively, and it outlives a dropped identity, as other branches may use
 * it. Only the branch's catalogs record which sequence serves the column.
 */
static void
mutation_identity(Oid relid, AlterTableCmd *action)
{
	Relation	rel = relation_open(relid, NoLock);
	Relation	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	HeapTuple	tuple = SearchSysCacheCopyAttName(relid, action->name);
	ParseState *pstate = make_parsestate(NULL);
	CatalogView *view = CatalogViewCurrent();
	Form_pg_attribute attr;
	List	   *options = NIL;

	pstate->p_sourcetext = debug_query_string;
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						action->name, RelationGetRelationName(rel))));
	attr = (Form_pg_attribute) GETSTRUCT(tuple);
	if (attr->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", action->name)));
	if (action->subtype == AT_AddIdentity)
	{
		Constraint *constraint = castNode(Constraint, action->def);
		CreateSeqStmt *create = makeNode(CreateSeqStmt);
		char	   *schema = get_namespace_name(RelationGetNamespace(rel));
		ObjectAddress sequence,
					column;

		if (!attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" must be declared NOT NULL before identity can be added",
							action->name, RelationGetRelationName(rel))));
		if (attr->attidentity)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is already an identity column",
							action->name, RelationGetRelationName(rel))));
		if (attr->atthasdef)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" already has a default value",
							action->name, RelationGetRelationName(rel))));

		/* As generateSerialSequence, in the shared schema of the same name. */
		foreach_node(DefElem, option, constraint->options)
		{
			if (!strcmp(option->defname, "sequence_name"))
				create->sequence = makeRangeVarFromNameList(castNode(List, option->arg));
			else
				options = lappend(options, option);
		}
		CatalogViewSwitch(NULL);
		PG_TRY();
		{
			if (!create->sequence)
				create->sequence = makeRangeVar(schema, ChooseRelationName(RelationGetRelationName(rel), action->name, "seq",
																			get_namespace_oid(schema, false), false), -1);
			create->options = lappend(options, makeDefElem("as", (Node *) makeTypeNameFromOid(attr->atttypid, -1), -1));
			create->ownerId = rel->rd_rel->relowner;
			create->for_identity = true;
			ObjectAddressSet(sequence, RelationRelationId, DefineSequence(pstate, create).objectId);
		}
		PG_FINALLY();
		{
			CatalogViewSwitch(view);
		}
		PG_END_TRY();
		CommandCounterIncrement();
		attr->attidentity = constraint->generated_when;
		ObjectAddressSubSet(column, RelationRelationId, relid, attr->attnum);
		recordDependencyOn(&sequence, &column, DEPENDENCY_INTERNAL);
	}
	else if (!attr->attidentity)
	{
		if (action->subtype != AT_DropIdentity || !action->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
							action->name, RelationGetRelationName(rel))));
		ereport(NOTICE,
				(errmsg("column \"%s\" of relation \"%s\" is not an identity column, skipping",
						action->name, RelationGetRelationName(rel))));
	}
	else if (action->subtype == AT_SetIdentity)
	{
		foreach_node(DefElem, option, castNode(List, action->def))
		{
			if (!strcmp(option->defname, "generated"))
				attr->attidentity = defGetInt32(option);
			else
				options = lappend(options, option);
		}
		if (options)
		{
			AlterSeqStmt *alter = makeNode(AlterSeqStmt);
			Oid			sequence = getIdentitySequence(rel, attr->attnum, false);

			CatalogViewSwitch(NULL);
			PG_TRY();
			{
				alter->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(sequence)), get_rel_name(sequence), -1);
				alter->options = options;
				alter->for_identity = true;
				(void) AlterSequence(pstate, alter);
			}
			PG_FINALLY();
			{
				CatalogViewSwitch(view);
			}
			PG_END_TRY();
		}
	}
	else
	{
		deleteDependencyRecordsForClass(RelationRelationId, getIdentitySequence(rel, attr->attnum, false),
										RelationRelationId, DEPENDENCY_INTERNAL);
		attr->attidentity = '\0';
	}
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);
	CommandCounterIncrement();
	heap_freetuple(tuple);
	table_close(attrelation, RowExclusiveLock);
	relation_close(rel, NoLock);
}

static void
native_alter_publish(Capture current, VersionRef working, const char *name, AlterTableStmt *statement, DGState *state, int oldcount, Oid caller)
{
	CatalogView *view = CatalogViewCreate("provisional native ALTER");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	DGState    *saved_source = native_mutation_source;
	DGState    *saved_destination = native_mutation_destination;
	Oid			saved_relation = native_mutation_relation;
	int			saved_width = native_mutation_width;
	bytea	  **members;
	char	  **kinds,
			  **names,
			  **payloads;
	Datum	   *children;
	StringInfoData json;
	int			count;
	int			used = 0;
	List	   *indexes = NIL,
			   *oldindexes = NIL;
	bool		saved_indexes = native_mutation_indexes;
	bool		saved_rewritten = native_mutation_rewritten;
	bool		rewrite = false;
	PLTree		tree = state->content;

	PG_TRY();
	{
		Oid			id;
		ListCell   *lc;
		List	   *commands = statement->cmds;

		provider_populate(view, current.hash, NULL);
		CatalogViewBeginMutation(view);
		CatalogViewSetMutationValidator(view, mutation_validate_tuple);
		CatalogViewSetStorageHandler(view, mutation_storage);
		native_mutation_indexes = false;
		native_mutation_view = view;
		native_mutation_source = state;
		native_mutation_destination = NULL;
		native_mutation_rewritten = false;
		id = CatalogViewLookupObject(view, RelationRelationId, name);
		native_mutation_relation = id;
		CatalogViewSwitch(view);

		foreach(lc, commands)
		{
			AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

			if (identity_action(action))
			{
				mutation_identity(id, action);
				continue;
			}
			if (action->subtype == AT_AddConstraint &&
				castNode(Constraint, action->def)->contype != CONSTR_CHECK && castNode(Constraint, action->def)->contype != CONSTR_NOTNULL && castNode(Constraint, action->def)->contype != CONSTR_UNIQUE && castNode(Constraint, action->def)->contype != CONSTR_PRIMARY && castNode(Constraint, action->def)->contype != CONSTR_FOREIGN)
				unsupported("native constraint storage codec");
			if (action->subtype == AT_AddIndex)
			{
				IndexStmt  *index = castNode(IndexStmt, action->def);
				ListCell   *cc;

				if (index->concurrent || index->whereClause || index->options || index->tableSpace ||
					(index->accessMethod && strcmp(index->accessMethod, "btree") && strcmp(index->accessMethod, "dolt_prolly_index")))
					unsupported("canonical index storage properties");
				index->accessMethod = pstrdup("dolt_prolly_index");
				foreach(cc, index->indexParams)
				{
					IndexElem  *element = lfirst_node(IndexElem, cc);

					/* A user operator class is local to this server. */
					if (element->opclass &&
						get_opclass_oid(DOLT_PROLLY_INDEX_AM_OID, element->opclass, false) >= FirstNormalObjectId)
						unsupported("canonical index with a user-defined operator class");
				}
				action->def = (Node *) transformIndexStmt(id, index, debug_query_string ? debug_query_string : "");
			}
			if (action->subtype == AT_AlterColumnType)
			{
				ColumnDef  *col = castNode(ColumnDef, action->def);
				Relation	rel = relation_open(id, NoLock);

				if (col->raw_default)
					(void) schema_expression_bind(view, id, RelationGetRelationName(rel), RelationGetDescr(rel), col->raw_default, -1);
				relation_close(rel, NoLock);
			}
			if (action->subtype == AT_AddColumn)
			{
				ColumnDef  *col = castNode(ColumnDef, action->def);
				ListCell   *cc;
				const char *typename = strVal(llast(col->typeName->names));

				/*
				 * PostgreSQL ignores an existing IF NOT EXISTS definition
				 * before transforming defaults or subsidiary constraints.
				 */
				if (action->missing_ok && get_attnum(id, col->colname) != InvalidAttrNumber)
					continue;
				if (col->identity || col->generated || !strcmp(typename, "serial") || !strcmp(typename, "serial2") || !strcmp(typename, "serial4") || !strcmp(typename, "serial8") || !strcmp(typename, "smallserial") || !strcmp(typename, "bigserial"))
					unsupported("native identity/sequence/generated column codec");
				foreach(cc, col->constraints)
				{
					Constraint *con = lfirst_node(Constraint, cc);

					if (con->contype != CONSTR_DEFAULT && con->contype != CONSTR_NULL && con->contype != CONSTR_NOTNULL && con->contype != CONSTR_CHECK && con->contype != CONSTR_PRIMARY && con->contype != CONSTR_UNIQUE && con->contype != CONSTR_FOREIGN)
						unsupported("native added-column constraint codec");
				}
			}
		}

		{
			AlterTableUtilityContext context = {0};
			PlannedStmt *plan = makeNode(PlannedStmt);
			AlterTableStmt *stmt = copyObject(statement);
			Oid			privileged;
			int			security;
			int			pathnest = NewGUCNestLevel();

			foreach_node(AlterTableCmd, action, stmt->cmds)
				if (identity_action(action))
				stmt->cmds = foreach_delete_current(stmt->cmds, action);
			plan->commandType = CMD_UTILITY;
			plan->utilityStmt = (Node *) stmt;
			context.pstmt = plan;
			context.queryString = debug_query_string ? debug_query_string : "";
			context.relid = id;
			GetUserIdAndSecContext(&privileged, &security);
			SetUserIdAndSecContext(caller, security);
			PG_TRY(caller);
			{
				set_config_option("search_path", api_search_path, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
				if (stmt->cmds)
					AlterTable(stmt, AlterTableGetLockLevel(stmt->cmds), &context);
			}
			PG_FINALLY(caller);
			{
				AtEOXact_GUC(true, pathnest);
				SetUserIdAndSecContext(privileged, security);
			}
			PG_END_TRY(caller);
		}
		CommandCounterIncrement();
		indexes = mutation_indexes(view, id);
		CatalogViewSwitch(NULL);
		run("SELECT t.objhash,t.objkind,CASE WHEN t.objkind IN ('table','index') THEN CASE WHEN convert_from(t.objpayload,'UTF8') IS JSON OBJECT THEN convert_from(t.objpayload,'UTF8')::jsonb->>'name' END END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(current.hash)), NULL, true);
		count = SPI_processed;
		members = palloc_array(bytea *, Max(count, 1));
		kinds = palloc_array(char *, Max(count, 1));
		names = palloc_array(char *, Max(count, 1));
		payloads = palloc_array(char *, Max(count, 1));
		children = palloc_array(Datum, Max(count + list_length(indexes), 1));
		for (int i = 0; i < count; i++)
		{
			members[i] = DatumGetByteaPCopy(cell(i, 1));
			kinds[i] = strcell(i, 2);
			names[i] = strcell(i, 3);
		}
		for (int i = 0; i < count; i++)
			if (names[i] && !strcmp(names[i], name))
				oldindexes = table_index_names(metadata(child(members[i], 1), "schema"), name);

		/*
		 * Serialize all surviving indexes, including newly allocated native
		 * handles.
		 */
		foreach(lc, indexes)
		{
			MutationIndex *item = lfirst(lc);
			char	   *old = NULL,
					   *definition;
			int		   *mapping,
						attributes;
			PLTree		empty;

			for (int i = 0; i < count; i++)
				if (!strcmp(kinds[i], "index") && !strcmp(names[i], item->name))
				{
					bytea	   *schema = child(members[i], 1);

					old = metadata(schema, "schema");
					item->resource = binding_relation(dg_current_branch(), item->name, schema);
				}
			if (!old)
				old = canonical_definition(item->name, item->owner, acldefault(OBJECT_TABLE, item->owner), "[]", "[]", "[]", item->sql);
			definition = mutation_columns(view, item->id, old, &mapping, &attributes);
			run("SELECT jsonb_set($1::jsonb,'{index_definition}',to_jsonb($2::text))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(definition), CStringGetTextDatum(item->sql)), NULL, true);
			definition = strcell(0, 1);
			dg_check(pl_build(state->store, NULL, 0, &empty));
			item->member = canonical_empty_member("index", item->name, definition, &empty);
			item->schema = child(item->member, 1);
		}
		initStringInfo(&json);
		appendStringInfoChar(&json, '[');
		for (int i = 0; i < count; i++)
		{
			payloads[i] = metadata(members[i], kinds[i]);
			if (!strcmp(kinds[i], "index") && name_selected(oldindexes, names[i]))
				continue;
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
					if (native_mutation_destination)
					{
						rewrite = true;
						tree = alter_rows(native_mutation_destination, native_mutation_width, attributes, mapping, NULL, NULL);
					}
					else if (rewrite)
						tree = alter_rows(state, oldcount, attributes, mapping, NULL, NULL);
				}

				if (!strcmp(kinds[i], "table"))
					definition = mutation_constraints(view, object, definition);
				if (!strcmp(names[i], name))
					definition = mutation_index_list(definition, indexes);

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
			if (used)
				appendStringInfoChar(&json, ',');
			if (!strcmp(kinds[i], "namespace"))
				appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", payloads[i]);
			else
				appendStringInfoString(&json, payloads[i]);
			children[used++] = PointerGetDatum(members[i]);
		}
		foreach(lc, indexes)
		{
			MutationIndex *item = lfirst(lc);

			if (used)
				appendStringInfoChar(&json, ',');
			appendStringInfoString(&json, metadata(item->member, "index"));
			children[used++] = PointerGetDatum(item->member);
		}
		appendStringInfoChar(&json, ']');
		run("SELECT jsonb_set($1::jsonb,'{members}',$2::jsonb)::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(json.data)), NULL, true);
		{
			bytea	   *database = canonical_database(strcell(0, 1), children, used);

			CatalogView *final = provider_view(database);

			foreach(lc, indexes)
			{
				MutationIndex *item = lfirst(lc);

				if (!OidIsValid(item->resource))
				{
					Relation	index = relation_open(CatalogViewLookupObject(final, RelationRelationId, item->name), NoLock);

					item->resource = dg_storage_allocate(index, dg_current_branch());
					relation_close(index, NoLock);
				}
				binding_record(dg_current_branch(), item->name, item->schema, item->resource);
			}
			move_ref(branch_ref("working"), working, database);
			advance_snapshot();
			if (rewrite || indexes != NIL || native_mutation_indexes)
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
		native_mutation_source = saved_source;
		native_mutation_destination = saved_destination;
		native_mutation_relation = saved_relation;
		native_mutation_width = saved_width;
		native_mutation_indexes = saved_indexes;
		native_mutation_rewritten = saved_rewritten;
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}

/* A provisional CREATE may make storage only for the relations it defines. */
static void
create_storage(Relation relation, CatalogViewStorageEffect effect)
{
	if (relation->rd_catalogview != native_mutation_view ||
		(relation->rd_rel->relam != DOLT_PROLLY_TABLE_AM_OID && relation->rd_rel->relam != DOLT_PROLLY_INDEX_AM_OID))
		unsupported("native CREATE storage outside prolly tables and indexes");
}

/* Run one ALTER TABLE of a provisional CREATE against its new relation. */
static void
create_alter(Oid id, AlterTableStmt *stmt, const char *query)
{
	AlterTableUtilityContext context = {0};
	PlannedStmt *plan = makeNode(PlannedStmt);

	plan->commandType = CMD_UTILITY;
	plan->utilityStmt = (Node *) stmt;
	context.pstmt = plan;
	context.queryString = query;
	context.relid = id;
	AlterTable(stmt, AlterTableGetLockLevel(stmt->cmds), &context);
	CommandCounterIncrement();
}

/*
 * Native CREATE TABLE runs against a private copy of the branch's catalogs,
 * as native ALTER does, so PostgreSQL itself interprets the statement. The
 * result is serialized into canonical table and index definitions; nothing
 * is published here. Sequences are database-global, as in Doltgres: serial
 * creates its sequence natively, without an owning column. Foreign keys are
 * returned for the caller to add once the table is published.
 */
static void
native_create_definitions(Capture current, CreateStmt *statement, const char *query, Oid caller,
						  const char *name, char **table, List **index_names,
						  List **index_definitions, List **foreign)
{
	CatalogView *view = CatalogViewCreate("provisional native CREATE");
	CatalogView *previous = CatalogViewCurrent();
	CatalogView *saved_mutation = native_mutation_view;
	Oid			saved_relation = native_mutation_relation;
	Oid			privileged;
	int			security;
	int			pathnest = NewGUCNestLevel();

	*index_names = *index_definitions = *foreign = NIL;
	GetUserIdAndSecContext(&privileged, &security);
	PG_TRY();
	{
		List	   *commands;
		List	   *indexes = NIL;
		List	   *identities = NIL;	/* (column name, sequence OID) */
		List	   *identity_sequences = NIL;
		RangeVar   *rv = NULL;
		Oid			id = InvalidOid;
		Oid			owner;
		Acl		   *acl;
		ListCell   *lc;
		char	   *definition;
		int		   *mapping;
		int			attributes;

		provider_populate(view, current.hash, NULL);
		CatalogViewBeginMutation(view);
		CatalogViewSetMutationValidator(view, mutation_validate_tuple);
		CatalogViewSetStorageHandler(view, create_storage);
		native_mutation_view = view;
		native_mutation_relation = InvalidOid;
		CatalogViewSwitch(view);
		SetUserIdAndSecContext(caller, security);
		set_config_option("search_path", api_search_path, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

		commands = transformCreateStmt(statement, query);
		while (commands != NIL)
		{
			Node	   *command = linitial(commands);

			commands = list_delete_first(commands);
			if (IsA(command, CreateStmt))
			{
				rv = ((CreateStmt *) command)->relation;
				id = DefineRelation((CreateStmt *) command, RELKIND_RELATION, InvalidOid, NULL, query).objectId;
				CatalogViewNameObject(id, name);
				native_mutation_relation = id;
				{
					/* Storage codecs vet column types before any definition. */
					Relation	rel = relation_open(id, NoLock);

					dg_validate_descriptor(RelationGetDescr(rel));
					relation_close(rel, NoLock);
				}
				CommandCounterIncrement();
			}
			else if (IsA(command, TableLikeClause))
				commands = list_concat(expandTableLikeClause(rv, (TableLikeClause *) command), commands);
			else if (IsA(command, CreateSeqStmt))
			{
				/* A sequence is shared by every branch: create it natively. */
				CreateSeqStmt *sequence = (CreateSeqStmt *) command;
				ParseState *pstate = make_parsestate(NULL);
				ListCell   *oc;

				foreach(oc, sequence->options)
					if (!strcmp(lfirst_node(DefElem, oc)->defname, "owned_by"))
						sequence->options = foreach_delete_current(sequence->options, oc);
				pstate->p_sourcetext = query;
				CatalogViewSwitch(NULL);
				{
					Oid			created = DefineSequence(pstate, sequence).objectId;

					if (sequence->for_identity)
						identity_sequences = lappend_oid(identity_sequences, created);
				}
				CatalogViewSwitch(view);
				CommandCounterIncrement();
			}
			else if (IsA(command, AlterSeqStmt))
			{
				/*
				 * serial's OWNED BY would tie a global sequence to one branch.
				 * An identity column still needs to find its sequence.
				 */
				AlterSeqStmt *alter = (AlterSeqStmt *) command;
				ListCell   *oc;
				Oid			sequence;

				CatalogViewSwitch(NULL);
				sequence = RangeVarGetRelid(alter->sequence, NoLock, false);
				CatalogViewSwitch(view);
				foreach(oc, alter->options)
				{
					DefElem    *option = lfirst_node(DefElem, oc);

					if (strcmp(option->defname, "owned_by"))
						unsupported("canonical CREATE sequence option");
					if (list_member_oid(identity_sequences, sequence))
						identities = lappend(identities, list_make2(llast(castNode(List, option->arg)), (void *) (uintptr_t) sequence));
				}
			}
			else if (IsA(command, IndexStmt))
			{
				AlterTableCmd *action = makeNode(AlterTableCmd);
				IndexStmt  *index = (IndexStmt *) command;

				if (index->accessMethod && strcmp(index->accessMethod, "btree") && strcmp(index->accessMethod, "dolt_prolly_index"))
					unsupported("canonical index storage properties");
				index->accessMethod = pstrdup("dolt_prolly_index");
				index->tableSpace = NULL;
				index->options = NIL;
				action->subtype = AT_AddIndex;
				action->def = (Node *) transformIndexStmt(id, index, query);
				indexes = lappend(indexes, action);
			}
			else if (IsA(command, AlterTableStmt))
			{
				AlterTableStmt *alter = (AlterTableStmt *) command;
				bool		foreign_only = true;

				foreach(lc, alter->cmds)
				{
					AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

					if (action->subtype != AT_AddConstraint || !IsA(action->def, Constraint) ||
						castNode(Constraint, action->def)->contype != CONSTR_FOREIGN)
						foreign_only = false;
				}
				if (foreign_only)
					*foreign = lappend(*foreign, alter);
				else
					create_alter(id, alter, query);
			}
			else
				unsupported("canonical CREATE auxiliary command");
		}
		/*
		 * An identity column finds its global sequence through an internal
		 * dependency, recorded in the branch's catalogs only.
		 */
		foreach(lc, identities)
		{
			List	   *identity = lfirst(lc);
			ObjectAddress sequence,
						column;

			ObjectAddressSet(sequence, RelationRelationId, (Oid) (uintptr_t) lsecond(identity));
			ObjectAddressSubSet(column, RelationRelationId, id, get_attnum(id, strVal(linitial(identity))));
			recordDependencyOn(&sequence, &column, DEPENDENCY_INTERNAL);
			CommandCounterIncrement();
		}
		if (indexes)
		{
			AlterTableStmt *alter = makeNode(AlterTableStmt);

			alter->relation = rv;
			alter->objtype = OBJECT_TABLE;
			alter->cmds = indexes;
			create_alter(id, alter, query);
		}
		AtEOXact_GUC(true, pathnest);
		SetUserIdAndSecContext(privileged, security);

		/* Serialize as native ALTER does, from the provisional catalogs. */
		{
			CatalogView *outer = CatalogViewSwitch(view);
			Relation	rel = relation_open(id, NoLock);

			owner = rel->rd_rel->relowner;
			relation_close(rel, NoLock);
			CatalogViewSwitch(outer);
		}
		acl = acldefault(OBJECT_TABLE, owner);
		definition = canonical_definition(name, owner, acl, "[]", "[]", "[]", NULL);
		definition = mutation_columns(view, id, definition, &mapping, &attributes);
		definition = mutation_constraints(view, id, definition);
		indexes = mutation_indexes(view, id);
		foreach(lc, indexes)
		{
			MutationIndex *item = lfirst(lc);
			char	   *index = canonical_definition(item->name, item->owner, acldefault(OBJECT_TABLE, item->owner), "[]", "[]", "[]", item->sql);

			index = mutation_columns(view, item->id, index, &mapping, &attributes);
			*index_names = lappend(*index_names, item->name);
			*index_definitions = lappend(*index_definitions, index);
		}
		*table = mutation_index_list(definition, indexes);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		native_mutation_view = saved_mutation;
		native_mutation_relation = saved_relation;
		SetUserIdAndSecContext(privileged, security);
		if (CatalogViewIsProvisional(view))
			CatalogViewRetireMutation(view);
		CatalogViewRelease(view);
	}
	PG_END_TRY();
}
