/* A native catalog tuple proposal is transient. Only portable attribute values
 * enter the schema tree; MVCC headers, TIDs and runtime OIDs never do. */
typedef struct AttributeProposal
{
	Oid			relation;
	HeapTuple	tuple;
}			AttributeProposal;

static void
attribute_proposal(Relation catalog, HeapTuple tuple, void *arg)
{
	AttributeProposal *proposal = arg;
	Form_pg_attribute attr = (Form_pg_attribute) GETSTRUCT(tuple);
	HeapTuple	before;
	TupleDesc	desc = RelationGetDescr(catalog);

	if (RelationGetRelid(catalog) != AttributeRelationId ||
		attr->attrelid != proposal->relation || attr->attnum <= 0 || proposal->tuple)
		unsupported("unexpected native catalog mutation");
	before = SearchSysCache2(ATTNUM, ObjectIdGetDatum(attr->attrelid), Int16GetDatum(attr->attnum));
	if (!HeapTupleIsValid(before))
		elog(ERROR, "native catalog mutation target disappeared");
	for (int i = 1; i <= desc->natts; i++)
	{
		bool		anull,
					bnull;
		Datum		a,
					b;
		Form_pg_attribute field = TupleDescAttr(desc, i - 1);

		if (i == Anum_pg_attribute_attstattarget || i == Anum_pg_attribute_attoptions)
			continue;
		a = heap_getattr(before, i, desc, &anull);
		b = heap_getattr(tuple, i, desc, &bnull);
		if (anull != bnull || (!anull && !datumIsEqual(a, b, field->attbyval, field->attlen)))
			unsupported("native mutation of an unencoded catalog field");
	}
	ReleaseSysCache(before);
	proposal->tuple = heap_copytuple(tuple);
}

static char *
alter_native_attribute(Relation definition, AlterTableCmd *cmd, const char *json)
{
	AttributeProposal proposal = {RelationGetRelid(definition), NULL};
	CatalogViewMutation mutation = {CatalogViewObjectContext(proposal.relation), attribute_proposal, &proposal};
	CatalogViewMutation *saved = CatalogViewSwitchMutation(&mutation);
	CatalogView *previous = CatalogViewSwitch(mutation.view);
	Relation	catalog;
	bool		statsnull,
				optionsnull;
	Datum		stats,
				options;
	int			att;
	char	   *result;
	char		nulls[5] = "    ";

	PG_TRY();
	{
		AlterTableColumnMetadata(definition, cmd);
	}
	PG_FINALLY();
	{
		CatalogViewSwitchMutation(saved);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	if (!proposal.tuple)
		elog(ERROR, "native metadata command produced no catalog tuple");
	catalog = table_open(AttributeRelationId, AccessShareLock);
	stats = heap_getattr(proposal.tuple, Anum_pg_attribute_attstattarget, RelationGetDescr(catalog), &statsnull);
	options = heap_getattr(proposal.tuple, Anum_pg_attribute_attoptions, RelationGetDescr(catalog), &optionsnull);
	att = ((Form_pg_attribute) GETSTRUCT(proposal.tuple))->attnum - 1;
	nulls[2] = statsnull ? 'n' : ' ';
	nulls[3] = optionsnull ? 'n' : ' ';
	/* Absent/default values have one normal form; option ordering is stable. */
	run("SELECT jsonb_set($1::jsonb,ARRAY['columns',$2::text], (($1::jsonb->'columns'->$2)-'statistics'-'options') || jsonb_strip_nulls(jsonb_build_object('statistics',$3::int,'options',(SELECT jsonb_agg(v ORDER BY v COLLATE \"C\") FROM unnest($4::text[]) v))))::text", 4, VC_TYPES(TEXTOID, INT4OID, INT4OID, TEXTARRAYOID), VC_ARGS(CStringGetTextDatum(json), Int32GetDatum(att), stats, options), nulls, true);
	result = strcell(0, 1);
	table_close(catalog, AccessShareLock);
	heap_freetuple(proposal.tuple);
	return result;
}

/* Branch-local metadata revisions and eager tuple-layout rewrites. */
static PLTree
alter_rows(DGState *state, int oldcount, int dropped)
{
	DGRows		rows = dg_rows(&state->content);
	PLEntry    *entries = palloc_array(PLEntry, Max(rows.count, 1));
	size_t		used = 0;
	int			count = oldcount + (dropped < 0 ? 1 : -1);

	for (size_t i = 0; i < rows.count; i++)
	{
		PLValue    *fields = palloc0_array(PLValue, Max(count, 1));
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		uint8	   *packed,
				   *value,
				   *key;
		size_t		length;
		PLHash		hash;
		int			out = 0;

		for (int j = 0; j < oldcount; j++)
			if (j != dropped)
				dg_check(pl_tuple_get(row, j, &fields[out++]));
		dg_check(pl_tuple_pack(fields, count, &packed, &length));
		hash = dg_rowhash(DG_SLICE(packed, length));
		key = palloc(PL_HASH_SIZE);
		memcpy(key, hash.bytes, PL_HASH_SIZE);
		value = palloc(length + 8);
		memcpy(value, rows.items[i].value.data, 8);
		memcpy(value + 8, packed, length);
		free(packed);
		pfree(fields);
		entries[i] = DG_ENTRY(DG_SLICE(key, PL_HASH_SIZE), DG_SLICE(value, length + 8));
	}
	qsort(entries, rows.count, sizeof(*entries), entry_compare);
	for (size_t i = 0; i < rows.count; i++)
	{
		if (used && !pl_compare(entries[used - 1].key, entries[i].key))
		{
			uint64		a = pl_read64(entries[used - 1].value.data),
						b = pl_read64(entries[i].value.data);

			if (entries[used - 1].value.len != entries[i].value.len || memcmp(entries[used - 1].value.data + 8, entries[i].value.data + 8, entries[i].value.len - 8))
				elog(ERROR, "canonical row hash collision");
			if (UINT64_MAX - a < b)
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("row occurrence count overflow")));
			pl_write64((uint8 *) entries[used - 1].value.data, a + b);
		}
		else
			entries[used++] = entries[i];
	}
	{
		PLTree		result;

		dg_check(pl_build(state->store, entries, used, &result));
		return result;
	}
}

/* Replace one descriptor, preserving every unrelated canonical member. */
static bytea *
alter_database(Capture current, const char *name, bytea *member)
{
	Datum	   *children;
	int			n;
	char	   *json;

	run("SELECT jsonb_set($1::jsonb,'{members}',(SELECT jsonb_agg(CASE WHEN v->>'name'=$2 THEN $3::jsonb ELSE v END) FROM jsonb_array_elements($1::jsonb->'members') v))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(current.json), CStringGetTextDatum(name), CStringGetTextDatum(metadata(member, "table"))), NULL, true);
	json = strcell(0, 1);
	run("SELECT CASE WHEN o.objkind='table' AND convert_from(o.objpayload,'UTF8')::jsonb->>'name'=$2 THEN $3 ELSE ch END FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1", 3, VC_TYPES(BYTEAOID, TEXTOID, BYTEAOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name), PointerGetDatum(member)), NULL, true);
	n = SPI_processed;
	children = palloc_array(Datum, Max(n, 1));
	for (int i = 0; i < n; i++)
		children[i] = cell(i, 1);
	return canonical_database(json, children, n);
}

/* One-time transition of legacy native schema capture. No user commit is made. */
static bool
alter_activate(const RangeVar *rv, bool missing)
{
	Oid			id = RangeVarGetRelid(rv, AccessShareLock, missing);
	Relation	rel;
	Oid			user;
	int			security,
				nest;

	if (!OidIsValid(id))
		return false;
	rel = relation_open(id, NoLock);
	if (rel->rd_tableam != dg_table_routine())
	{
		relation_close(rel, NoLock);
		return false;
	}
	GetUserIdAndSecContext(&user, &security);
	if (!object_ownercheck(RelationRelationId, id, user))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, rv->relname);
	relation_close(rel, NoLock);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		Capture		current;

		nest = begin_api();
		lock_workspace();
		ensure_repository();
		if (!read_ref("views/enabled").target)
		{
			current = capture(true);
			move_ref(branch_ref("working"), read_ref(branch_ref("working")), current.hash);
			move_ref("views/enabled", empty_ref, read_ref(branch_ref("heads")).target);
			advance_snapshot();
		}
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	} PG_END_TRY();
	UpdateActiveSnapshotCommandId();
	return true;
}

bool
dg_catalog_alter(Node *statement)
{
	RangeVar   *rv;
	AlterTableCmd *cmd = NULL;
	CommentStmt *comment = NULL;
	Oid			user,
				id;
	int			security,
				nest;
	bool		pushed = false,
				missing = false;
	char	   *column = NULL,
			   *name;
	Relation	definition;

	if (rangevar_projection_hook || catalog_api_depth)
		return false;
	if (!IsA(statement, AlterTableStmt) && !IsA(statement, CommentStmt))
		return false;
	if (!dg_catalog_active() && (!IsA(statement, AlterTableStmt) || ((AlterTableStmt *) statement)->objtype != OBJECT_TABLE || list_length(((AlterTableStmt *) statement)->cmds) != 1))
		return false;
	if (IsA(statement, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *) statement;

		if (stmt->objtype == OBJECT_TABLE && list_length(stmt->cmds) > 1 && dg_catalog_active())
		{
			ListCell   *lc;

			/*
			 * Metadata actions have native left-to-right semantics. Reopen a
			 * fresh immutable definition for each action; SQL rollback covers
			 * all publications if a later native validator fails.
			 */
			foreach(lc, stmt->cmds)
			{
				AlterTableCmd *action = lfirst_node(AlterTableCmd, lc);

				if (action->subtype != AT_SetStatistics && action->subtype != AT_SetOptions && action->subtype != AT_ResetOptions)
					unsupported("mixed canonical ALTER action families");
			}
			foreach(lc, stmt->cmds)
			{
				AlterTableStmt *single = copyObject(stmt);

				single->cmds = list_make1(lfirst(lc));
				if (!dg_catalog_alter((Node *) single))
					elog(ERROR, "canonical metadata action not handled");
				if (ActiveSnapshotSet())
					UpdateActiveSnapshotCommandId();
			}
			return true;
		}
		if (stmt->objtype != OBJECT_TABLE || list_length(stmt->cmds) != 1)
			unsupported("canonical ALTER requires one table action");
		cmd = linitial_node(AlterTableCmd, stmt->cmds);
		if (cmd->subtype != AT_AddColumn && cmd->subtype != AT_DropColumn &&
			cmd->subtype != AT_SetStatistics && cmd->subtype != AT_SetOptions && cmd->subtype != AT_ResetOptions)
			return false;
		if (!dg_catalog_active())
		{
			Oid			nativeid = RangeVarGetRelid(stmt->relation, AccessShareLock, stmt->missing_ok);
			Relation	native;
			bool		prolly;

			if (!OidIsValid(nativeid))
				return false;
			native = relation_open(nativeid, NoLock);
			prolly = native->rd_tableam == dg_table_routine();
			relation_close(native, AccessShareLock);
			if (!prolly)
				return false;
		}
		if (cmd->behavior != DROP_RESTRICT)
			unsupported("canonical column DROP CASCADE");
		rv = stmt->relation;
		missing = stmt->missing_ok;
	}
	else
	{
		List	   *parts;

		comment = copyObject((CommentStmt *) statement);
		if (comment->comment && !comment->comment[0])
			comment->comment = NULL;
		if (comment->objtype != OBJECT_TABLE && comment->objtype != OBJECT_COLUMN)
			return false;
		parts = copyObject((List *) comment->object);
		if (comment->objtype == OBJECT_COLUMN)
		{
			column = pstrdup(strVal(llast(parts)));
			parts = list_truncate(parts, list_length(parts) - 1);
		}
		rv = makeRangeVarFromNameList(parts);
	}
	if (rv->catalogname || rv->dolt_revision)
		unsupported("revision-qualified canonical DDL");
	if (XactReadOnly)
		PreventCommandIfReadOnly("ALTER TABLE");
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (!dg_catalog_active() && !alter_activate(rv, missing))
	{
		if (pushed)
			PopActiveSnapshot();
		return false;
	}
	id = RangeVarGetRelid(rv, NoLock, missing);
	if (!OidIsValid(id))
	{
		if (pushed)
			PopActiveSnapshot();
		return true;
	}
	if (!CatalogViewObjectContext(id))
		unsupported("canonical ALTER of non-versioned relation");
	definition = relation_open(id, NoLock);
	if (definition->rd_rel->relkind != RELKIND_RELATION)
		unsupported("canonical ALTER of non-table");
	GetUserIdAndSecContext(&user, &security);
	if (!object_ownercheck(RelationRelationId, id, user))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, rv->relname);
	name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(definition)), RelationGetRelationName(definition));
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		VersionRef	working;
		Capture		current;
		Relation	source,
					target;
		DGState    *state;
		bytea	   *descriptor,
				   *schema,
				   *content,
				   *member,
				   *database,
				   *raw;
		char	   *json,
				   *payload;
		Oid			resource;
		int			att = -1,
					oldcount = RelationGetDescr(definition)->natts;
		bool		rewrite = cmd && (cmd->subtype == AT_AddColumn || cmd->subtype == AT_DropColumn),
					skip = false;
		PLTree		tree;

		nest = begin_api();
		run("SELECT 1 FROM pg_event_trigger WHERE evtenabled<>'D'", 0, NULL, NULL, NULL, true);
		if (SPI_processed)
			unsupported("canonical ALTER with event triggers");
		source = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
		if (RelationGetRelid(source) != id)
			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("canonical ALTER target changed; retry statement")));
		resource = source->rd_lockInfo.lockRelId.relId;
		state = dg_load(source, SnapshotSelf);
		tree = state->content;
		if (cmd && cmd->subtype == AT_DropColumn)
			column = cmd->name;
		if (column || (cmd && cmd->subtype == AT_AddColumn))
		{
			const char *wanted = column ? column : ((ColumnDef *) cmd->def)->colname;

			for (int i = 0; i < oldcount; i++)
				if (!strcmp(wanted, NameStr(TupleDescAttr(RelationGetDescr(source), i)->attname)))
					att = i;
			if (column && att < 0)
			{
				if (cmd && cmd->missing_ok)
					skip = true;
				else
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", wanted)));
			}
			if (!column && att >= 0)
			{
				if (cmd->missing_ok)
					skip = true;
				else
					ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN), errmsg("column \"%s\" already exists", wanted)));
			}
		}
		if (cmd && cmd->subtype == AT_DropColumn && !skip)
		{
			ListCell   *lc;

			foreach(lc, dg_storage_indexes(source))
			{
				Relation	index = lfirst(lc);

				for (int i = 0; i < index->rd_index->indnatts; i++)
					if (index->rd_index->indkey.values[i] == att + 1)
						unsupported("dropping an indexed column");
			}
		}
		dg_storage_close_relation(source);
		working = read_ref(branch_ref("working"));
		current = capture(true);
		run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind='table' AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(current.hash), CStringGetTextDatum(name)), NULL, true);
		if (SPI_processed != 1)
			elog(ERROR, "canonical ALTER member missing");
		descriptor = DatumGetByteaPCopy(cell(0, 1));
		json = metadata(child(descriptor, 1), "schema");
		if (!skip)
		{
			if (comment)
			{
				char	   *path = column ? psprintf("{columns,%d,comment}", att) : "{comment}";

				run("SELECT jsonb_set($1::jsonb,$2::text[],coalesce(to_jsonb($3::text),'null'::jsonb))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(path), comment->comment ? CStringGetTextDatum(comment->comment) : (Datum) 0), comment->comment ? NULL : "  n", true);
				json = strcell(0, 1);
			}
			else if (cmd->subtype == AT_SetStatistics || cmd->subtype == AT_SetOptions || cmd->subtype == AT_ResetOptions)
			{
				json = alter_native_attribute(definition, cmd, json);
			}
			else if (cmd->subtype == AT_AddColumn)
			{
				ColumnDef  *col = (ColumnDef *) cmd->def;
				TupleDesc	desc;
				Form_pg_attribute a;
				char	   *added;

				if (col->constraints || col->raw_default || col->cooked_default || col->identity || col->generated || col->collClause || col->storage || col->compression)
					unsupported("canonical ADD COLUMN requires a plain nullable column");
				if (oldcount >= MaxHeapAttributeNumber)
					ereport(ERROR, (errcode(ERRCODE_TOO_MANY_COLUMNS), errmsg("tables can have at most %d columns", MaxHeapAttributeNumber)));
				desc = BuildDescForRelation(list_make1(col));
				dg_validate_descriptor(desc);
				a = TupleDescAttr(desc, 0);
				run("SELECT jsonb_build_object('name',$1::text,'comment',NULL,'type',t.typname,'typmod',$3::int,'notnull',false,'collation',CASE WHEN $4::oid=0 THEN NULL ELSE (SELECT jsonb_build_array(n.nspname,c.collname,c.collprovider,c.collisdeterministic,c.collcollate,c.collctype,c.colllocale,c.collversion) FROM pg_collation c JOIN pg_namespace n ON n.oid=c.collnamespace WHERE c.oid=$4) END)::text FROM pg_type t WHERE t.oid=$2 AND t.typnamespace='pg_catalog'::regnamespace", 4, VC_TYPES(TEXTOID, OIDOID, INT4OID, OIDOID), VC_ARGS(CStringGetTextDatum(col->colname), ObjectIdGetDatum(a->atttypid), Int32GetDatum(a->atttypmod), ObjectIdGetDatum(a->attcollation)), NULL, true);
				if (SPI_processed != 1)
					unsupported("canonical ADD COLUMN user-defined types");
				added = strcell(0, 1);
				run("SELECT jsonb_set($1::jsonb,'{columns}',($1::jsonb->'columns')||jsonb_build_array($2::jsonb))::text", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(added)), NULL, true);
				json = strcell(0, 1);
				tree = alter_rows(state, oldcount, -1);
			}
			else
			{
				run("SELECT jsonb_set(jsonb_set($1::jsonb,'{columns}',($1::jsonb->'columns')-$2),'{constraints}',coalesce((SELECT jsonb_agg(v) FROM jsonb_array_elements($1::jsonb->'constraints') v WHERE v->>1<>$3),'[]'::jsonb))::text", 3, VC_TYPES(TEXTOID, INT4OID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), Int32GetDatum(att), CStringGetTextDatum(psprintf("NOT NULL %s", quote_identifier(column)))), NULL, true);
				json = strcell(0, 1);
				tree = alter_rows(state, oldcount, att);
			}
			schema = put("schema", json, NULL, 0);
			content = dg_history_export_tree(&tree);
			payload = metadata(descriptor, "table");
			raw = palloc(VARHDRSZ + PL_HASH_SIZE);
			SET_VARSIZE(raw, VARHDRSZ + PL_HASH_SIZE);
			memcpy(VARDATA(raw), tree.root.bytes, PL_HASH_SIZE);
			run("SELECT jsonb_set(jsonb_set($1::jsonb,'{schema}',to_jsonb(encode(sha256(convert_to($2,'UTF8')),'hex'))),'{content}',to_jsonb($3::text))::text", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(payload), CStringGetTextDatum(json), CStringGetTextDatum(hex(raw))), NULL, true);
			member = put("table", strcell(0, 1), VC_ARGS(PointerGetDatum(schema), PointerGetDatum(content)), 2);
			database = alter_database(current, name, member);
			binding_record(dg_current_branch(), name, schema, resource);
			move_ref(branch_ref("working"), working, database);
			advance_snapshot();
			if (rewrite)
			{
				target = dg_catalog_write_table(dg_current_branch(), name, ACL_UPDATE);
				dg_history_install(target, state, tree);
				dg_storage_close_relation(target);
				UpdateActiveSnapshotCommandId();
				move_ref(branch_ref("working"), read_ref(branch_ref("working")), capture(true).hash);
			}
		}
		dg_release(state);
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		relation_close(definition, NoLock);
		if (pushed)
			PopActiveSnapshot();
	} PG_END_TRY();
	return true;
}
