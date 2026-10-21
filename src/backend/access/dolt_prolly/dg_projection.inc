/* Explicit retained-version materialization. No catalog writes during lookup.
 * Runtime objects are rebuildable; their OIDs never enter canonical history. */
static bool projection_building;
bool
dg_projection_building(void)
{
	return projection_building;
}

bool
dg_projected_namespace(Oid namespaceId)
{
	Relation	catalog;
	TableScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;
	char	   *name;

	if (!IsUnderPostmaster || !OidIsValid(namespaceId))
		return false;
	name = get_namespace_name(namespaceId);
	if (!name)
		return false;
	if (strncmp(name, "pg_dolt_p_", 10))
	{
		pfree(name);
		return false;
	}
	pfree(name);
	catalog = table_open(DoltProjectionRelationId, AccessShareLock);
	scan = table_beginscan_catalog(catalog, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_dolt_projection entry = (Form_pg_dolt_projection) GETSTRUCT(tuple);

		if (entry->runtime_namespace == namespaceId)
		{
			found = true;
			break;
		}
	}
	table_endscan(scan);
	table_close(catalog, AccessShareLock);
	return found;
}
void
dg_projection_write_check(Relation rel)
{
	if (!projection_building && dg_projected_namespace(RelationGetNamespace(rel)))
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("retained catalog projections are read-only")));
}

void
dg_projection_relation_check(Oid relationId, int lockmode)
{
	if (!projection_building && OidIsValid(relationId) && lockmode > AccessShareLock && dg_projected_namespace(get_rel_namespace(relationId)))
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("retained catalog projections are read-only")));
}

/* Called before normal namespace/permission/relation-lock processing. Each RTE
 * receives an ordinary distinct OID, so two versions can coexist in one plan. */
const RangeVar *
dg_project_relation(const RangeVar *input)
{
	const char *sep;
	char	   *dbname,
			   *revision,
			   *physical = NULL;
	bytea	   *commit;
	RangeVar   *result;
	int			nest;
	bool		pushed = false;
	List	   *logical_path = NIL;
	ListCell   *lc;

	if (!input || projection_building)
		return input;
	if (input->dolt_revision)
	{
		dbname = get_database_name(MyDatabaseId);
		revision = input->dolt_revision;
		if (input->catalogname && strcmp(input->catalogname, dbname))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("AS OF cannot be combined with a different database qualifier")));
	}
	else
	{
		if (!input->catalogname)
			return input;
		sep = strpbrk(input->catalogname, "@/");
		if (!sep)
			return input;
		dbname = pnstrdup(input->catalogname, sep - input->catalogname);
		if (strcmp(dbname, get_database_name(MyDatabaseId)))
			return input;
		/* The lexer truncates identifiers before binding. Never interpret a
		 * possibly truncated full hash as a valid abbreviated hash. */
		if (strlen(input->catalogname) == NAMEDATALEN - 1)
			ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("revision-qualified identifier may have been truncated"), errhint("Use AS OF 'full hash', or a shorter unambiguous revision qualifier.")));
		revision = pstrdup(sep + 1);
		if (!*revision || !input->schemaname)
			ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("revision-qualified references require database@revision.schema.table")));
	}
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}

	if (input->schemaname)
		logical_path = list_make1(input->schemaname);
	else
		foreach(lc, fetch_search_path(false)) logical_path = lappend(logical_path, get_namespace_name(lfirst_oid(lc)));
	nest = begin_api();
	if (!input->dolt_revision && !read_ref("heads/main").target)
	{
		end_api(nest);
		if (pushed) PopActiveSnapshot();
		return input;
	}
	commit = resolve(revision);
	foreach(lc, logical_path)
	{

		run("SELECT n.nspname FROM pg_dolt_projection p JOIN pg_namespace n ON n.oid=p.runtime_namespace WHERE p.commit_hash=$1 AND p.logical_namespace=$2 AND EXISTS(SELECT FROM pg_class c WHERE c.relnamespace=n.oid AND c.relname=$3)", 3, VC_TYPES(BYTEAOID, TEXTOID, TEXTOID), VC_ARGS(PointerGetDatum(commit), CStringGetTextDatum((char *) lfirst(lc)), CStringGetTextDatum(input->relname)), NULL, true);
		if (SPI_processed)
		{
			physical = strcell(0, 1);
			break;
		}
	}

	if (!physical)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("revision relation is absent or not materialized: %s.%s", revision, input->schemaname ? input->schemaname : "public"), errhint("Run SELECT dolt_catalog_materialize('revision') in a writable transaction before querying it.")));
	end_api(nest);
	if (pushed)
		PopActiveSnapshot();
	result = (RangeVar *) copyObject(input);
	result->catalogname = dbname;
	result->schemaname = physical;
	result->dolt_revision = NULL;
	return result;
}

static char *
projection_schema(bytea *commit, const char *logical)
{
	run("SELECT n.nspname FROM pg_dolt_projection p JOIN pg_namespace n ON n.oid=p.runtime_namespace WHERE p.commit_hash=$1 AND p.logical_namespace=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(commit), CStringGetTextDatum(logical)), NULL, true);
	if (!SPI_processed)
		elog(ERROR, "missing projected namespace %s", logical);
	return strcell(0, 1);
}

Datum
dolt_catalog_materialize(PG_FUNCTION_ARGS)
{
	int			nest,
				i,
				j,
				n,
				nn;
	bytea	   *commit,
			   *db;
	char	  **namespaces,
			  **descriptors;
	bool		previous = projection_building;
	char	   *answer;

	standalone(fcinfo, false);
	if (IsolationUsesXactSnapshot())
		unsupported("catalog materialization requires READ COMMITTED isolation");
	nest = begin_api();
	commit = resolve(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	db = child(commit, 1);
	answer = hex(commit);

	/*
	 * Serialize only builders of the same retained generation, not other
	 * branches.
	 */
	LockDatabaseObject(DoltProjectionRelationId, pl_read32((uint8 *) VARDATA_ANY(commit)), pl_read32((uint8 *) VARDATA_ANY(commit) + 4), ExclusiveLock);
	PopActiveSnapshot();
	PushCopiedSnapshot(GetLatestSnapshot());
	run("SELECT 1 FROM pg_dolt_projection WHERE commit_hash=$1 LIMIT 1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(commit)), NULL, true);
	if (SPI_processed)
	{
		end_api(nest);
		PG_RETURN_TEXT_P(cstring_to_text(answer));
	}
	run("SELECT v->'definition'->>'name' FROM pg_dolt_object o,jsonb_array_elements(convert_from(o.objpayload,'UTF8')::jsonb->'members') v WHERE o.objhash=$1 AND v->>'kind'='namespace' ORDER BY 1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(db)), NULL, true);
	nn = SPI_processed;
	namespaces = palloc(sizeof(char *) * Max(nn, 1));
	for (i = 0; i < nn; i++)
		namespaces[i] = strcell(i, 1);
	if (!nn)
		unsupported("materialization of a bootstrap state without namespaces");
	run("SELECT convert_from(s.objpayload,'UTF8') FROM pg_dolt_object db CROSS JOIN LATERAL unnest(db.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch JOIN pg_dolt_object s ON s.objhash=t.objchildren[1] WHERE db.objhash=$1 AND t.objkind='table' ORDER BY convert_from(s.objpayload,'UTF8')::jsonb->>'name'", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(db)), NULL, true);
	n = SPI_processed;
	descriptors = palloc(sizeof(char *) * Max(n, 1));
	for (i = 0; i < n; i++)
		descriptors[i] = strcell(i, 1);
	projection_building = true;
	PG_TRY();
	{
		set_config_option("allow_system_table_mods", "on", PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
		for (i = 0; i < nn; i++)
		{
			char	   *physical;
			Oid			oid;

			run("SELECT 'pg_dolt_p_'||substr(encode(sha256($1||convert_to($2,'UTF8')),'hex'),1,40)", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(commit), CStringGetTextDatum(namespaces[i])), NULL, true);
			physical = strcell(0, 1);
			oid = NamespaceCreate(physical, GetUserId(), false);
			run("INSERT INTO pg_dolt_projection VALUES($1,$2,$3)", 3, VC_TYPES(OIDOID, BYTEAOID, TEXTOID), VC_ARGS(ObjectIdGetDatum(oid), PointerGetDatum(commit), CStringGetTextDatum(namespaces[i])), NULL, false);
			advance_snapshot();
		}
		for (i = 0; i < n; i++)
		{
			char	   *logical = json_value(descriptors[i], "name"),
					   *ns,
					   *relname,
					   *target,
					   *physical;
			StringInfoData ddl;
			char	  **columns;
			int			nc;
			Oid			oid;
			Relation	rel;
			DGState    *state;
			PLTree		contents;

			run("SELECT (parse_ident($1))[1],(parse_ident($1))[2]", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(logical)), NULL, true);
			ns = strcell(0, 1);
			relname = strcell(0, 2);
			physical = projection_schema(commit, ns);
			target = quote_qualified_identifier(physical, relname);
			run("SELECT quote_ident(v->>'name')||' '||format_type(to_regtype('pg_catalog.'||quote_ident(v->>'type')), (v->>'typmod')::int)|| CASE WHEN v->'collation' IS NOT NULL AND v->'collation'<>'null'::jsonb THEN ' COLLATE '||quote_ident(v->'collation'->>0)||'.'||quote_ident(v->'collation'->>1) ELSE '' END || CASE WHEN (v->>'notnull')::boolean THEN ' NOT NULL' ELSE '' END FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(descriptors[i])), NULL, true);
			nc = SPI_processed;
			columns = palloc(sizeof(char *) * Max(nc, 1));
			for (j = 0; j < nc; j++)
				columns[j] = strcell(j, 1);
			initStringInfo(&ddl);
			appendStringInfo(&ddl, "CREATE TABLE %s (", target);
			for (j = 0; j < nc; j++)
			{
				if (j)
					appendStringInfoChar(&ddl, ',');
				appendStringInfoString(&ddl, columns[j]);
			}
			appendStringInfoString(&ddl, ") USING dolt_prolly_table");
			run(ddl.data, 0, NULL, NULL, NULL, false);
			advance_snapshot();
			oid = get_relname_relid(relname, get_namespace_oid(physical, false));
			rel = relation_open(oid, AccessExclusiveLock);
			state = dg_load(rel, SnapshotSelf);
			contents = dg_history_import(state->store, table_chunk(db, logical));
			dg_history_install(rel, state, contents);
			dg_release(state);
			relation_close(rel, NoLock);

			/*
			 * Only primary/unique constraints occur in this captured schema
			 * subset. NOT NULL was already installed in the column
			 * definitions above.
			 */
			run("SELECT quote_ident(v->>0)||' '||(v->>1) FROM jsonb_array_elements($1::jsonb->'constraints') v WHERE v->>1 LIKE 'PRIMARY KEY%' OR v->>1 LIKE 'UNIQUE%'", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(descriptors[i])), NULL, true);
			nc = SPI_processed;
			columns = palloc(sizeof(char *) * Max(nc, 1));
			for (j = 0; j < nc; j++)
				columns[j] = strcell(j, 1);
			for (j = 0; j < nc; j++)
			{
				run(psprintf("ALTER TABLE %s ADD CONSTRAINT %s", target, columns[j]), 0, NULL, NULL, NULL, false);
				advance_snapshot();
			}
			run("SELECT v #>> '{}' FROM jsonb_array_elements($1::jsonb->'indexes') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(descriptors[i])), NULL, true);
			nc = SPI_processed;
			columns = palloc(sizeof(char *) * Max(nc, 1));
			for (j = 0; j < nc; j++)
				columns[j] = strcell(j, 1);
			for (j = 0; j < nc; j++)
			{
				RawStmt    *raw = linitial_node(RawStmt, raw_parser(columns[j], RAW_PARSE_DEFAULT));
				IndexStmt  *index;
				PlannedStmt *wrapper;

				if (!IsA(raw->stmt, IndexStmt))
					elog(ERROR, "invalid retained index definition");
				index = (IndexStmt *) raw->stmt;
				/* Constraint creation already built its index. */
				if (OidIsValid(get_relname_relid(index->idxname, get_namespace_oid(physical, false))))
					continue;
				index->relation = makeRangeVar(physical, relname, -1);
				wrapper = makeNode(PlannedStmt);
				wrapper->commandType = CMD_UTILITY;
				wrapper->utilityStmt = (Node *) index;
				wrapper->stmt_location = -1;
				wrapper->stmt_len = 0;
				ProcessUtility(wrapper, columns[j], false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver, NULL);
				advance_snapshot();
			}

		}
	}
	PG_FINALLY();
	{
		projection_building = previous;
	} PG_END_TRY();
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}
