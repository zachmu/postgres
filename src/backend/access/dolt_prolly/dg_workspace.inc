/* Workspace commands (reset, clean, rm, table checkout, named add), branch
 * and tag management, and commit-graph helpers. See doc/doltgres/workspace-commands.md.
 * Included by dg_version.c. */

/* ---- Database state composition ----
 *
 * A database state object lists member objects (namespaces, tables, indexes).
 * compose_database builds a new state from |base|, replacing the members of
 * the selected tables (and their indexes) with the versions found in |source|
 * (or removing them when |source| lacks them), and adding namespaces that the
 * selected tables need. */

typedef struct Member
{
	bytea	   *hash;
	char	   *kind;
	char	   *name;			/* qualified table/index name or namespace
								 * name */
	char	   *payload;
}			Member;

static Member *
database_members(bytea *db, int *count)
{
	Member	   *result;
	int			i;

	if (!db)
	{
		*count = 0;
		return NULL;
	}
	run("SELECT o.objhash,o.objkind,CASE WHEN o.objkind='namespace' THEN NULL ELSE convert_from(o.objpayload,'UTF8')::jsonb->>'name' END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1 ORDER BY o.objkind,o.objhash", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(db)), NULL, true);
	*count = SPI_processed;
	result = palloc0(sizeof(Member) * Max(*count, 1));
	for (i = 0; i < *count; i++)
	{
		result[i].hash = DatumGetByteaPCopy(cell(i, 1));
		result[i].kind = strcell(i, 2);
		result[i].name = strcell(i, 3);
	}
	for (i = 0; i < *count; i++)
	{
		result[i].payload = metadata(result[i].hash, result[i].kind);
		if (!result[i].name)
			result[i].name = json_value(result[i].payload, "name");
	}
	return result;
}

/* Index member names that belong to a table, from its schema definitions. */
static List *
table_index_names(const char *schema_json, const char *table)
{
	List	   *result = NIL;
	char	   *ns = strVal(linitial(stringToQualifiedNameList(table, NULL)));
	int			i;

	run("SELECT substring(d FROM 'INDEX (\"[^\"]+\"|[^ ]+) ON ') FROM jsonb_array_elements_text($1::jsonb->'indexes') d", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema_json)), NULL, true);
	for (i = 0; i < SPI_processed; i++)
	{
		char	   *raw = strcell(i, 1);

		if (raw)
			result = lappend(result, quote_qualified_identifier(ns, strip_identifier(raw)));
	}
	return result;
}

static bool
name_selected(List *names, const char *name)
{
	ListCell   *lc;

	foreach(lc, names)
		if (!strcmp((char *) lfirst(lc), name))
		return true;
	return false;
}

/* Expand selected tables to their index members in |members|. */
static List *
selected_objects(Member * members, int count, List *tables)
{
	List	   *result = list_copy(tables);
	int			i;

	for (i = 0; i < count; i++)
		if (!strcmp(members[i].kind, "table") && name_selected(tables, members[i].name))
			result = list_concat(result, table_index_names(metadata(child(members[i].hash, 1), "schema"), members[i].name));
	return result;
}

static bytea *
compose_database(bytea *base, bytea *source, List *tables)
{
	int			nbase,
				nsource,
				i,
				used = 0;
	Member	   *bm = database_members(base, &nbase),
			   *sm = database_members(source, &nsource);
	List	   *drop = selected_objects(bm, nbase, tables),
			   *take = selected_objects(sm, nsource, tables);
	Datum	   *hashes = palloc(sizeof(Datum) * (nbase + nsource + 1));
	StringInfoData json;
	ListCell   *lc;

	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (i = 0; i < nbase; i++)
	{
		if (strcmp(bm[i].kind, "namespace") && (name_selected(drop, bm[i].name) || name_selected(take, bm[i].name)))
			continue;
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(bm[i].kind, "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", bm[i].payload);
		else
			appendStringInfoString(&json, bm[i].payload);
		hashes[used++] = PointerGetDatum(bm[i].hash);
	}
	for (i = 0; i < nsource; i++)
	{
		bool		wanted = false;

		if (!strcmp(sm[i].kind, "namespace"))
		{
			/* A selected table's schema must exist in the result. */
			int			j;

			foreach(lc, take)
			{
				char	   *ns = strVal(linitial(stringToQualifiedNameList((char *) lfirst(lc), NULL)));

				if (!strcmp(ns, sm[i].name))
					wanted = true;
			}
			for (j = 0; wanted && j < nbase; j++)
				if (!strcmp(bm[j].kind, "namespace") && !strcmp(bm[j].name, sm[i].name))
					wanted = false;
		}
		else
			wanted = name_selected(take, sm[i].name);
		if (!wanted)
			continue;
		if (used)
			appendStringInfoChar(&json, ',');
		if (!strcmp(sm[i].kind, "namespace"))
			appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", sm[i].payload);
		else
			appendStringInfoString(&json, sm[i].payload);
		hashes[used++] = PointerGetDatum(sm[i].hash);
	}
	appendStringInfoString(&json, "]}");
	return canonical_database(json.data, hashes, used);
}

/* Resolve table arguments of a workspace command against the given states. */
static List *
table_arguments(char **args, int n, int first, const char *json_a, const char *json_b)
{
	List	   *result = NIL;
	int			i;

	for (i = first; i < n; i++)
	{
		if (!strcmp(args[i], "."))
			return NIL;
		result = lappend(result, resolve_table_argument(args[i], json_a, json_b, true));
	}
	return result;
}

static List *
state_tables(const char *json)
{
	List	   *result = NIL;
	int			i;

	run("SELECT v->>'name' FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'='table' ORDER BY v->>'name' COLLATE \"C\"", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json)), NULL, true);
	for (i = 0; i < SPI_processed; i++)
		result = lappend(result, strcell(i, 1));
	return result;
}

/* ---- Live table installation and removal ---- */

/* Publish the complete canonical definition first, then install matching
 * contents under that definition. All changes share the caller's SQL commit.
 * NIL tables replaces every table, including roots left by a deleted branch. */
static void
replace_working(const char *branch, bytea *database, List *tables)
{
	int			count;
	Member	   *members;
	CatalogView *view;
	List	   *selected;
	VersionRef	working;

	advance_snapshot();
	members = database_members(database, &count);
	view = provider_view(database);
	selected = tables ? selected_objects(members, count, tables) : NIL;
	working = read_ref(psprintf("working/%s", branch));

	binding_prepare(branch, database, false);
	advance_snapshot();
	for (int i = 0; i < count; i++)
	{
		Oid			resource;
		Relation	definition;
		bytea	   *schema;

		if (strcmp(members[i].kind, "table") && strcmp(members[i].kind, "index"))
			continue;
		if (tables && !name_selected(selected, members[i].name))
			continue;
		schema = child(members[i].hash, 1);
		resource = binding_relation(branch, members[i].name, schema);
		if (OidIsValid(resource))
			continue;
		definition = relation_open(CatalogViewLookupObject(view, RelationRelationId, members[i].name), NoLock);
		resource = dg_storage_allocate(definition, branch);
		binding_record(branch, members[i].name, schema, resource);
		relation_close(definition, NoLock);
	}
	move_ref(psprintf("working/%s", branch), working, database);
	if (!strcmp(branch, dg_current_branch()) && !read_ref("views/enabled").target)
		move_ref("views/enabled", empty_ref, read_ref(psprintf("heads/%s", branch)).target);
	advance_snapshot();
	for (int i = 0; i < count; i++)
	{
		Relation	rel;
		DGState    *state;
		PLTree		tree;

		if (strcmp(members[i].kind, "table") ||
			(tables && !name_selected(tables, members[i].name)))
			continue;
		rel = dg_catalog_write_table(branch, members[i].name, ACL_UPDATE);
		state = dg_load(rel, SnapshotSelf);
		tree = dg_history_import(state->store, table_chunk(database, members[i].name));
		dg_history_install(rel, state, tree);
		dg_release(state);
		dg_storage_close_relation(rel);
		UpdateActiveSnapshotCommandId();
	}
}

static void
restore_table_on(const char *branch, bytea *db, const char *name)
{
	bytea	   *current = !strcmp(branch, dg_current_branch()) ?
		capture(true).hash : read_ref(psprintf("working/%s", branch)).target;
	List	   *tables = list_make1(pstrdup(name));
	bytea	   *result;

	advance_snapshot();
	result = compose_database(current, db, tables);

	replace_working(branch, result, tables);
}

static void
restore_table(bytea *db, const char *name)
{
	restore_table_on(dg_current_branch(), db, name);
}

/* Drop a live table through ordinary DDL, which the branch-local catalog
 * provider handles once branch views are active. */
static void
drop_table(const char *name)
{
	run_context(psprintf("DROP TABLE %s", name), 0, NULL, NULL, NULL, false);
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
}

/* Persist the live working state and make its objects visible to the API
 * snapshot (capture publishes them under a temporary latest snapshot). */
static Capture
capture_visible(void)
{
	Capture		current = capture(true);

	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	return current;
}

/* ---- dolt_add with table names ---- */

static void
stage_tables(List *tables)
{
	VersionRef	staged = read_ref(branch_ref("staged")),
				working = read_ref(branch_ref("working"));
	Capture		current = capture_visible();
	bytea	   *base = staged.target ? staged.target : child(read_ref(branch_ref("heads")).target, 1);
	bytea	   *result = compose_database(base, current.hash, tables);

	move_ref(branch_ref("staged"), staged, result);
	move_ref(branch_ref("working"), working, current.hash);
}

/* ---- dolt_reset ---- */

Datum
dolt_reset(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		hard = false,
				soft = false;
	char	   *target = NULL;
	List	   *names = NIL;
	VersionRef	head,
				staged;
	bytea	   *headdb;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--hard"))
			hard = true;
		else if (!strcmp(args[i], "--soft"))
			soft = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			names = lappend(names, args[i]);
	}
	if (hard && soft)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: --hard and --soft are mutually exclusive options.")));
	head = read_ref(branch_ref("heads"));
	staged = read_ref(branch_ref("staged"));
	headdb = child(head.target, 1);
	if (hard || soft)
	{
		bytea	   *commit;

		if (list_length(names) > 1)
			unsupported("reset accepts at most one revision");
		target = names ? (char *) linitial(names) : "HEAD";
		commit = resolve(target);
		if (hard)
		{
			bytea	   *db = child(commit, 1);

			replace_working(dg_current_branch(), db, NIL);
			move_ref(branch_ref("staged"), staged, db);
			finish_merge();
		}
		if (!same(commit, head.target))
			move_ref(branch_ref("heads"), head, commit);
	}
	else if (names == NIL || (list_length(names) == 1 && !strcmp(linitial(names), ".")))
	{
		/* Unstage everything. */
		move_ref(branch_ref("staged"), staged, headdb);
	}
	else
	{
		char	   *stagedjson = staged_state(),
				   *headjson = head_state();
		bool		is_table = resolve_table_argument((char *) linitial(names), stagedjson, headjson, false) != NULL;

		if (!is_table && list_length(names) == 1)
		{
			/*
			 * Mixed reset to a revision: HEAD and staging move, working
			 * stays.
			 */
			bytea	   *commit = resolve((char *) linitial(names));

			move_ref(branch_ref("heads"), head, commit);
			move_ref(branch_ref("staged"), staged, child(commit, 1));
		}
		else
		{
			List	   *tables = table_arguments(args, n, 0, stagedjson, headjson);

			move_ref(branch_ref("staged"), staged, compose_database(staged.target ? staged.target : headdb, headdb, tables));
		}
	}
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_reset_noargs(PG_FUNCTION_ARGS)
{
	return dolt_reset(fcinfo);
}

/* ---- dolt_clean: drop untracked tables ---- */

Datum
dolt_clean(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		dry = false;
	List	   *names = NIL,
			   *tracked,
			   *live,
			   *untracked = NIL;
	ListCell   *lc;
	char	   *workingjson;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--dry-run"))
			dry = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			names = lappend(names, args[i]);
	}
	workingjson = working_state();
	tracked = list_concat(state_tables(head_state()), state_tables(staged_state()));
	live = state_tables(workingjson);
	if (names)
		foreach(lc, names)
	{
		char	   *name = resolve_table_argument((char *) lfirst(lc), workingjson, workingjson, false);

		if (!name)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table not found: '%s'", (char *) lfirst(lc))));
		if (!name_selected(tracked, name))
			untracked = lappend(untracked, name);
	}
	else
		foreach(lc, live)
		if (!name_selected(tracked, (char *) lfirst(lc)))
		untracked = lappend(untracked, lfirst(lc));
	if (!dry)
		foreach(lc, untracked)
			drop_table((char *) lfirst(lc));
	end_api(nest);
	PG_RETURN_INT64(0);
}

Datum
dolt_clean_noargs(PG_FUNCTION_ARGS)
{
	return dolt_clean(fcinfo);
}

/* ---- dolt_rm ---- */

Datum
dolt_rm(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	bool		cached = false;
	List	   *tables = NIL;
	ListCell   *lc;
	VersionRef	staged;
	char	   *stagedjson;

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	stagedjson = staged_state();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--cached"))
			cached = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			tables = lappend(tables, resolve_table_argument(args[i], stagedjson, working_state(), true));
	}
	if (!tables)
		unsupported("dolt_rm requires table names");
	staged = read_ref(branch_ref("staged"));
	move_ref(branch_ref("staged"), staged, compose_database(staged.target ? staged.target : child(read_ref(branch_ref("heads")).target, 1), NULL, tables));
	if (!cached)
		foreach(lc, tables)
			drop_table((char *) lfirst(lc));
	end_api(nest);
	PG_RETURN_INT64(0);
}

/* ---- Table checkout: restore working tables from HEAD ---- */

static void
checkout_tables(char **args, int n)
{
	char	   *headjson = head_state(),
			   *workingjson = working_state();
	bytea	   *headdb = child(read_ref(branch_ref("heads")).target, 1);
	List	   *headtables = state_tables(headjson);
	int			i;
	StringInfoData missing;

	initStringInfo(&missing);
	for (i = 0; i < n; i++)
	{
		char	   *name = resolve_table_argument(args[i], headjson, workingjson, false);

		if (!name)
		{
			if (missing.len)
				appendStringInfoChar(&missing, '\n');
			appendStringInfo(&missing, "error: tablespec '%s' did not match any table(s) known to dolt", args[i]);
			continue;
		}
		if (name_selected(headtables, name))
			restore_table(headdb, name);
		else
			drop_table(name);
	}
	if (missing.len)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("%s", missing.data)));
}

/* ---- Branch and tag management ---- */

static void
delete_branch_refs(const char *name)
{
	const char *prefixes[] = {"heads", "working", "staged", "merge"};
	int			i;

	for (i = 0; i < lengthof(prefixes); i++)
	{
		VersionRef	r = read_ref(psprintf("%s/%s", prefixes[i], name));

		if (r.target)
			move_ref(psprintf("%s/%s", prefixes[i], name), r, NULL);
	}
	run("DELETE FROM pg_catalog.pg_dolt_binding WHERE bindbranch=$1", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(name)), NULL, false);
}

static void
create_branch_refs(const char *name, bytea *commit, bytea *working_db)
{
	move_ref(psprintf("heads/%s", name), read_ref(psprintf("heads/%s", name)), commit);
	move_ref(psprintf("working/%s", name), read_ref(psprintf("working/%s", name)), working_db ? working_db : child(commit, 1));
	move_ref(psprintf("staged/%s", name), read_ref(psprintf("staged/%s", name)), child(commit, 1));
	replace_working(name, working_db ? working_db : child(commit, 1), NIL);
}

static void
require_clean_branch(const char *name)
{
	VersionRef	head = read_ref(psprintf("heads/%s", name)),
				working = read_ref(psprintf("working/%s", name)),
				staged = read_ref(psprintf("staged/%s", name));
	bytea	   *db = child(head.target, 1);

	if ((working.target && !same(working.target, db)) || (staged.target && !same(staged.target, db)))
		unsupported("renaming or copying a branch with uncommitted changes");
	if (!strcmp(name, dg_current_branch()) && state_differs(working_state(), head_state()))
		unsupported("renaming or copying the session branch with uncommitted changes");
}

/* Extended dolt_branch: -d/-D delete, -m rename, -c copy, -f force. */
static bool
branch_management(char **args, int n)
{
	bool		del = false,
				rename = false,
				copy = false,
				force = false;
	List	   *names = NIL;
	ListCell   *lc;
	int			i;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-d") || !strcmp(args[i], "--delete"))
			del = true;
		else if (!strcmp(args[i], "-D"))
			del = true;
		else if (!strcmp(args[i], "-m") || !strcmp(args[i], "--move"))
			rename = true;
		else if (!strcmp(args[i], "-c") || !strcmp(args[i], "--copy"))
			copy = true;
		else if (!strcmp(args[i], "-f") || !strcmp(args[i], "--force"))
			force = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			names = lappend(names, args[i]);
	}
	if (!del && !rename && !copy)
	{
		if (force)
			unsupported("-f without -d, -m or -c");
		return false;
	}
	if ((del ? 1 : 0) + (rename ? 1 : 0) + (copy ? 1 : 0) > 1)
		unsupported("combining -d, -m and -c");
	lock_workspace();
	ensure_repository();
	if (del)
	{
		if (!names)
			unsupported("-d requires branch names");
		foreach(lc, names)
		{
			char	   *name = (char *) lfirst(lc);

			if (!*name)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("error: cannot delete empty branch name")));
			if (!strcmp(name, dg_current_branch()))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("Cannot delete checked out branch '%s'", name)));
			if (!read_ref(psprintf("heads/%s", name)).target)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("branch not found: %s", name)));
			if (!force)
				dg_branch_require_unused(name);
			delete_branch_refs(name);
		}
		return true;
	}
	if (list_length(names) != 2)
		unsupported(rename ? "-m requires old and new branch names" : "-c requires source and destination branch names");
	{
		char	   *src = (char *) linitial(names),
				   *dst = (char *) lsecond(names);
		VersionRef	srchead = read_ref(psprintf("heads/%s", src));

		valid_name(dst);
		if (!srchead.target)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("branch not found: %s", src)));
		if (read_ref(psprintf("heads/%s", dst)).target)
		{
			if (!force)
				ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("branch already exists: %s", dst)));
			if (!strcmp(dst, dg_current_branch()))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("Cannot overwrite checked out branch '%s'", dst)));
			delete_branch_refs(dst);
			advance_snapshot();
		}
		if (rename && strcmp(src, dg_current_branch()) && !force)
			dg_branch_require_unused(src);
		require_clean_branch(src);
		create_branch_refs(dst, srchead.target, NULL);
		if (rename)
		{
			advance_snapshot();
			delete_branch_refs(src);
			if (!strcmp(src, dg_current_branch()))
				dg_select_branch(dst);
		}
	}
	return true;
}

/* ---- Commit graph helpers ---- */

static int64
ancestor_difference(bytea *of, bytea *minus)
{
	run("WITH RECURSIVE a(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM a JOIN pg_dolt_object_visible o ON o.objhash=a.h), b(h) AS (SELECT $2::bytea UNION SELECT unnest(o.objchildren[2:]) FROM b JOIN pg_dolt_object_visible o ON o.objhash=b.h) SELECT count(*) FROM (SELECT h FROM a EXCEPT SELECT h FROM b) x", 2, VC_TYPES(BYTEAOID, BYTEAOID), VC_ARGS(PointerGetDatum(of), PointerGetDatum(minus)), NULL, true);
	return DatumGetInt64(cell(0, 1));
}

Datum
dolt_count_commits(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *from = NULL,
			   *to = NULL;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	bytea	   *a,
			   *b;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--from") && i + 1 < n)
			from = args[++i];
		else if (!strcmp(args[i], "--to") && i + 1 < n)
			to = args[++i];
		else if (!strncmp(args[i], "--from=", 7))
			from = args[i] + 7;
		else if (!strncmp(args[i], "--to=", 5))
			to = args[i] + 5;
		else
			unsupported(args[i]);
	}
	if (!from || !*from)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("missing from ref")));
	if (!to || !*to)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("missing to ref")));
	nest = begin_api();
	a = resolve(from);
	b = resolve(to);
	values[0] = Int64GetDatum(ancestor_difference(b, a));
	values[1] = Int64GetDatum(ancestor_difference(a, b));
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

Datum
dolt_branch_status(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bytea	   *base;

	if (n < 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("dolt_branch_status requires a base revision")));
	nest = begin_api();
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	base = resolve(args[0]);
	for (i = 1; i < n; i++)
	{
		bytea	   *branch = resolve(args[i]);
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		values[0] = CStringGetTextDatum(args[i]);
		values[1] = DirectFunctionCall1(int8_numeric, Int64GetDatum(ancestor_difference(branch, base)));
		values[2] = DirectFunctionCall1(int8_numeric, Int64GetDatum(ancestor_difference(base, branch)));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
	return (Datum) 0;
}

Datum
dolt_merge_base(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	bytea	   *a = resolve(text_to_cstring(PG_GETARG_TEXT_PP(0))),
			   *b = resolve(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	char	   *answer = hex(merge_base(a, b));

	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

Datum
dolt_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PG_VERSION "-dolt"));
}

Datum
dolt_hashof_table(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *working = working_state();
	char	   *name = resolve_table_argument(text_to_cstring(PG_GETARG_TEXT_PP(0)), working, working, true);
	char	   *answer;

	run("SELECT v->>'content' FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'='table' AND v->>'name'=$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(working), CStringGetTextDatum(name)), NULL, true);
	answer = strcell(0, 1);
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

Datum
dolt_hashof_db(PG_FUNCTION_ARGS)
{
	int			nest = begin_api();
	char	   *revision = PG_NARGS() ? text_to_cstring(PG_GETARG_TEXT_PP(0)) : "WORKING";
	bytea	   *db;
	char	   *answer;

	if (!pg_strcasecmp(revision, "WORKING"))
		db = capture_visible().hash;
	else
	{
		bool		working;

		db = revision_database(revision, &working);
	}
	answer = hex(db);
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

Datum
dolt_hashof_db_noargs(PG_FUNCTION_ARGS)
{
	return dolt_hashof_db(fcinfo);
}

/* ---- dolt_reflog ---- */

Datum
dolt_reflog(PG_FUNCTION_ARGS)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *filter = NULL;
	bool		all = false;

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--all"))
			all = true;
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else if (filter)
			unsupported(args[i]);
		else
			filter = args[i];
	}
	nest = begin_api();
	run("SELECT CASE WHEN l.reflogname LIKE 'heads/%' THEN 'refs/heads/' || substr(l.reflogname,7) WHEN l.reflogname LIKE 'tags/%' THEN 'refs/tags/' || substr(l.reflogname,6) ELSE 'refs/' || l.reflogname END,l.reflogtime,encode(CASE WHEN o.objkind='tag' THEN o.objchildren[1] ELSE l.reflogtarget END,'hex'),m.meta->>'message' FROM pg_catalog.pg_dolt_reflog l JOIN pg_dolt_object_visible o ON o.objhash=l.reflogtarget LEFT JOIN pg_dolt_commit_visible m ON m.objhash=CASE WHEN o.objkind='tag' THEN o.objchildren[1] ELSE l.reflogtarget END WHERE (l.reflogname LIKE 'heads/%' OR l.reflogname LIKE 'tags/%' OR $3) AND ($1::text IS NULL OR l.reflogname=$1 OR l.reflogname='heads/' || $1 OR l.reflogname='tags/' || $1 OR 'refs/' || l.reflogname=$1 OR $2) ORDER BY l.reflogtime DESC,l.reflogname", 3, VC_TYPES(TEXTOID, BOOLOID, BOOLOID), VC_ARGS(filter ? CStringGetTextDatum(filter) : (Datum) 0, BoolGetDatum(filter == NULL), BoolGetDatum(all)), filter ? NULL : "n  ", true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

Datum
dolt_reflog_noargs(PG_FUNCTION_ARGS)
{
	return dolt_reflog(fcinfo);
}

/* ---- dolt_revert and dolt_cherry_pick: three-way application of one
 * commit's changes (or their inverse) onto the branch head ---- */

static Datum
apply_commit(FunctionCallInfo fcinfo, bool revert)
{
	int			n,
				nest,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *author = NULL,
			   *email = NULL;
	List	   *revisions = NIL;
	ListCell   *lc;
	VersionRef	head;
	bytea	   *commit = NULL;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	standalone(fcinfo, true);
	nest = begin_api();
	lock_workspace();
	ensure_repository();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--author") && i + 1 < n)
			author_option(args[++i], &author, &email);
		else if (args[i][0] == '-')
			unsupported(args[i]);
		else
			revisions = lappend(revisions, args[i]);
	}
	if (!revisions)
		unsupported(revert ? "dolt_revert requires a revision" : "dolt_cherry_pick requires a revision");
	if (!revert && list_length(revisions) != 1)
		unsupported("cherry-picking more than one commit");
	head = read_ref(branch_ref("heads"));
	if (!same(capture_visible().hash, child(head.target, 1)) || !same(read_ref(branch_ref("staged")).target, child(head.target, 1)))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("cannot %s with uncommitted changes", revert ? "revert" : "cherry-pick")));
	foreach(lc, revisions)
	{
		bytea	   *target = resolve((char *) lfirst(lc)),
				   *parent,
				   *targetdb,
				   *parentdb,
				   *database;
		char	   *meta = metadata(target, "commit"),
				   *message;
		StringInfoData conflicts,
					violations;

		if (parent_count(target) < 1)
			unsupported("reverting or cherry-picking a root commit");
		parent = child(target, 2);
		targetdb = child(target, 1);
		parentdb = child(parent, 1);
		head = read_ref(branch_ref("heads"));
		same_schema(metadata(child(head.target, 1), "database"), metadata(targetdb, "database"));
		same_schema(metadata(child(head.target, 1), "database"), metadata(parentdb, "database"));
		initStringInfo(&conflicts);
		initStringInfo(&violations);
		if (revert)
			install_database(targetdb, child(head.target, 1), parentdb, &conflicts, &violations);
		else
			install_database(parentdb, child(head.target, 1), targetdb, &conflicts, &violations);
		if (conflicts.len || violations.len)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("%s resulted in conflicts; resolve them with dolt_merge", revert ? "revert" : "cherry-pick")));
		database = capture_visible().hash;
		message = revert ? psprintf("Revert \"%s\"", json_value(meta, "message")) : json_value(meta, "message");
		commit = commit_object(database, head.target, NULL, message, author, email, GetCurrentTimestamp());
		move_ref(branch_ref("heads"), head, commit);
		move_ref(branch_ref("staged"), read_ref(branch_ref("staged")), database);
		move_ref(branch_ref("working"), read_ref(branch_ref("working")), database);
		advance_snapshot();
	}
	values[0] = CStringGetTextDatum(hex(commit));
	values[1] = Int64GetDatum(0);
	values[2] = Int64GetDatum(0);
	values[3] = Int64GetDatum(0);
	end_api(nest);
	return command_record(fcinfo, values, nulls);
}

Datum
dolt_revert(PG_FUNCTION_ARGS)
{
	return apply_commit(fcinfo, true);
}

Datum
dolt_cherry_pick(PG_FUNCTION_ARGS)
{
	return apply_commit(fcinfo, false);
}
