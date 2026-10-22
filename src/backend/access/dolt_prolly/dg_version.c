/* Native version-control command slice; see doc/doltgres/version-commands.md. */
#include "dg.h"
#include "nodes/makefuncs.h"
#include "access/doltcatalog.h"
#include "catalog/namespace.h"
#include "commands/dbcommands.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/regproc.h"
#include "utils/varlena.h"
#include "parser/parse_expr.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "optimizer/optimizer.h"
#include "access/xact.h"
#include "access/doltbranch.h"
#include "catalog/pg_dolt_ref.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "access/relation.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_description.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/portal.h"
#include "tcop/pquery.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#define USER_NAMESPACE "n.nspname !~ '^pg_' AND n.nspname <> 'information_schema'"

typedef struct VersionRef
{
	bytea	   *target;
	bytea	   *token;
} VersionRef;
typedef struct Capture
{
	char	   *json;
	bytea	   *hash;
} Capture;
static const VersionRef empty_ref = {NULL, NULL};

/* Typed argument vectors live for the enclosing block and are consumed synchronously. */
#define VC_TYPES(...) ((Oid[]) {__VA_ARGS__})
#define VC_ARGS(...) ((Datum[]) {__VA_ARGS__})

static char *
branch_ref(const char *prefix)
{
	return psprintf("%s/%s", prefix, dg_current_branch());
}
static bytea *merge_parent(void);
static void finish_merge(void);

static void
run(const char *sql, int n, Oid *types, Datum *args, const char *nulls, bool ro)
{
	MemoryContext c = CurrentMemoryContext;

	SPI_execute_with_args(sql, n, types, args, nulls, ro, 0);
	MemoryContextSwitchTo(c);
}
static Datum
cell(int row, int col)
{
	bool		null;
	Datum		d = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col, &null);

	if (null)
		return (Datum) 0;
	return datumCopy(d, TupleDescAttr(SPI_tuptable->tupdesc, col - 1)->attbyval, TupleDescAttr(SPI_tuptable->tupdesc, col - 1)->attlen);
}
static char *
strcell(int row, int col)
{
	char	   *s = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col);

	return s ? pstrdup(s) : NULL;
}
static int
begin_api(void)
{
	MemoryContext c = CurrentMemoryContext;
	int			nest = NewGUCNestLevel();

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("version-control prototype requires superuser")));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "cannot connect version-control SPI");
	MemoryContextSwitchTo(c);
	PushCopiedSnapshot(GetActiveSnapshot());
	set_config_option("search_path", "pg_catalog, pg_temp", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	return nest;
}
static void
end_api(int nest)
{
	PopActiveSnapshot();
	SPI_finish();
	AtEOXact_GUC(true, nest);
}
static void
standalone(FunctionCallInfo fcinfo, bool commits)
{
	PlannedStmt *stmt;
	Plan	   *plan;
	FuncExpr   *call = NULL;

	if (!ActivePortal || list_length(ActivePortal->stmts) != 1)
		goto bad;
	stmt = linitial_node(PlannedStmt, ActivePortal->stmts);
	if (stmt->commandType != CMD_SELECT)
		goto bad;
	plan = stmt->planTree;
	if (plan->qual || plan->initPlan || outerPlan(plan) || innerPlan(plan))
		goto bad;
	if (IsA(plan, Result) && list_length(plan->targetlist) == 1)
	{
		Node	   *expr = (Node *) linitial_node(TargetEntry, plan->targetlist)->expr;

		if (IsA(expr, FuncExpr))
			call = (FuncExpr *) expr;
	}
	else if (IsA(plan, FunctionScan) && list_length(((FunctionScan *) plan)->functions) == 1)
	{
		Node	   *expr = linitial_node(RangeTblFunction, ((FunctionScan *) plan)->functions)->funcexpr;
		ListCell   *lc;

		if (IsA(expr, FuncExpr))
			call = (FuncExpr *) expr;
		foreach(lc, plan->targetlist) if (!IsA(lfirst_node(TargetEntry, lc)->expr, Var))
			goto bad;
	}
	if (!call || call->funcid != fcinfo->flinfo->fn_oid)
		goto bad;
	{
		ListCell   *lc;

		foreach(lc, call->args)
			if (!IsA(lfirst(lc), Const) && !IsA(lfirst(lc), Param) && !IsA(lfirst(lc), ArrayExpr))
			goto bad;
	}
	if (commits)
		PreventInTransactionBlock(true, "version-control command");
	return;
bad:
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("version-control mutation requires a standalone SELECT function call")));
}
static char **
arguments(FunctionCallInfo fcinfo, int *count)
{
	Datum	   *values;
	bool	   *nulls;
	char	  **args;
	int			i;

	if (PG_NARGS() == 0)
	{
		*count = 0;
		return NULL;
	}
	if (PG_ARGISNULL(0))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("command arguments cannot be null")));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(0), TEXTOID, -1, false, TYPALIGN_INT, &values, &nulls, count);
	args = palloc(sizeof(char *) * Max(*count, 1));
	for (i = 0; i < *count; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("command arguments cannot be null")));
		args[i] = TextDatumGetCString(values[i]);
	}
	return args;
}
static void
unsupported(const char *arg)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported version-control option or operation: %s", arg)));
}
static char *
hex(bytea *v)
{
	return TextDatumGetCString(DirectFunctionCall2(binary_encode, PointerGetDatum(v), CStringGetTextDatum("hex")));
}
static bytea *
unhex(const char *s)
{
	bytea	   *v = DatumGetByteaP(DirectFunctionCall2(binary_decode, CStringGetTextDatum(s), CStringGetTextDatum("hex")));

	if (VARSIZE_ANY_EXHDR(v) != 32)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid commit hash")));
	return v;
}
static bool
same(bytea *a, bytea *b)
{
	return a && b && VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b) && !memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a));
}
static VersionRef
read_ref(const char *name)
{
	VersionRef	r = {0};
	Oid			types[] = {TEXTOID};
	Datum		args[] = {CStringGetTextDatum(name)};

	run("SELECT reftarget,reftoken FROM pg_catalog.pg_dolt_ref WHERE refname=$1", 1, types, args, NULL, true);
	if (SPI_processed)
	{
		r.target = (bytea *) DatumGetPointer(cell(0, 1));
		r.token = (bytea *) DatumGetPointer(cell(0, 2));
	}
	return r;
}
static void
move_ref(const char *name, VersionRef old, bytea *target)
{
	Oid			types[] = {TEXTOID, BYTEAOID, BYTEAOID};
	Datum		args[] = {CStringGetTextDatum(name), PointerGetDatum(old.token), PointerGetDatum(target)};
	char		nulls[] = "   ";

	if (!old.token)
		nulls[1] = 'n';
	if (!target)
		nulls[2] = 'n';
	run("SELECT pg_catalog.dolt_manifest_cas($1,$2,$3)", 3, types, args, nulls, false);
}
static ArrayType *
links(Datum *hashes, int count)
{
	return count ? construct_array(hashes, count, BYTEAOID, -1, false, TYPALIGN_INT) : construct_empty_array(BYTEAOID);
}
static bytea *
put(const char *kind, const char *json, Datum *hashes, int count)
{
	Oid			types[] = {TEXTOID, TEXTOID, BYTEAARRAYOID};
	Datum		args[] = {CStringGetTextDatum(kind), CStringGetTextDatum(json), PointerGetDatum(links(hashes, count))};

	if ((!strcmp(kind, "schema") || !strcmp(kind, "namespace")) && count == 0)
		return dg_schema_store(kind, json);

	run("SELECT pg_catalog.dolt_manifest_put($1,convert_to(($2::jsonb)::text,'UTF8'),$3)", 3, types, args, NULL, false);
	return (bytea *) DatumGetPointer(cell(0, 1));
}
static char *
metadata(bytea *hash, const char *kind)
{
	Oid			types[] = {BYTEAOID, TEXTOID};
	Datum		args[] = {PointerGetDatum(hash), CStringGetTextDatum(kind)};

	if (!strcmp(kind, "schema") || !strcmp(kind, "namespace"))
		return dg_schema_read(hash, kind);

	run("SELECT convert_from(objpayload,'UTF8') FROM pg_catalog.pg_dolt_object WHERE objhash=$1 AND objkind=$2", 2, types, args, NULL, true);
	if (!SPI_processed)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("revision does not name a visible %s object", kind)));
	return strcell(0, 1);
}
static bytea *
child(bytea *hash, int index)
{
	Oid			types[] = {BYTEAOID, INT4OID};
	Datum		args[] = {PointerGetDatum(hash), Int32GetDatum(index)};

	run("SELECT objchildren[$2] FROM pg_catalog.pg_dolt_object WHERE objhash=$1", 2, types, args, NULL, true);
	if (!SPI_processed || !cell(0, 1))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("revision has no requested ancestor or state")));
	return (bytea *) DatumGetPointer(cell(0, 1));
}
static void
json_field(StringInfo b, const char *key, const char *value, bool comma)
{
	if (comma)
		appendStringInfoChar(b, ',');
	escape_json(b, key);
	appendStringInfoChar(b, ':');
	escape_json(b, value);
}

/* Canonical branch membership is authoritative once branch views are active. */
static bytea *
catalog_database(const char *branch)
{
	VersionRef	ref;

	if (!read_ref("views/enabled").target)
		return NULL;
	ref = read_ref(psprintf("working/%s", branch));
	if (ref.target)
		return ref.target;
	ref = read_ref(psprintf("heads/%s", branch));
	return ref.target ? child(ref.target, 1) : NULL;
}
static bool
catalog_contains(bytea *db, const char *name)
{
	if (!db)
		return true;
	run("SELECT 1 FROM pg_dolt_object d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index') AND convert_from(t.objpayload,'UTF8')::jsonb->>'name'=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(db), CStringGetTextDatum(name)), NULL, true);
	return SPI_processed != 0;
}

/* Fixed schema locks make ordinary relcache descriptors safe for this capture.
 * AccessShare relation locks keep DDL out without stopping row writers. */
static Capture
capture_inner(bool persist, bool all_catalogs)
{
	Oid			catalogs[] = {RelationRelationId, AttributeRelationId, IndexRelationId, NamespaceRelationId, TypeRelationId, ProcedureRelationId, ConstraintRelationId, TriggerRelationId, PolicyRelationId, AttrDefaultRelationId, CollationRelationId, AuthIdRelationId, DescriptionRelationId, OperatorRelationId};
	Capture		result = {0};
	bytea	   *membership = all_catalogs ? NULL : catalog_database(dg_current_branch());
	Oid		   *relations;
	char	  **namespaces;
	int			nrel,
				nns,
				i;
	Datum	   *members;
	int			used = 0;
	StringInfoData json;

	for (i = 0; i < lengthof(catalogs); i++)
		LockRelationOid(catalogs[i], ShareLock);
	if (IsolationUsesXactSnapshot())
		unsupported("live capture under Repeatable Read or Serializable (historical schema binding is not implemented)");
	PushActiveSnapshot(GetLatestSnapshot());
	run("SELECT c.oid FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE " USER_NAMESPACE " AND c.relpersistence<>'t' ORDER BY c.oid", 0, NULL, NULL, NULL, true);
	nrel = SPI_processed;
	relations = palloc(sizeof(Oid) * Max(nrel, 1));
	for (i = 0; i < nrel; i++)
		relations[i] = DatumGetObjectId(cell(i, 1));
	if (membership)
	{
		int			kept = 0;

		for (i = 0; i < nrel; i++)
		{
			Oid			id = relations[i];
			char	   *name = quote_qualified_identifier(get_namespace_name(get_rel_namespace(id)), get_rel_name(id));

			if (catalog_contains(membership, name))
				relations[kept++] = id;
		}
		nrel = kept;
	}
	for (i = 0; i < nrel; i++)
		LockRelationOid(relations[i], AccessShareLock);
	PopActiveSnapshot();
	PushActiveSnapshot(GetLatestSnapshot());
	/* Reject unsupported definitions instead of producing incomplete commits. */
	run("SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE " USER_NAMESPACE " AND c.relpersistence<>'t' AND (c.relkind NOT IN ('r','i') OR c.relpersistence<>'p' OR c.relam NOT IN (SELECT oid FROM pg_catalog.pg_am WHERE amname IN ('dolt_prolly_table','dolt_prolly_index')) OR c.relrowsecurity OR c.relispartition OR c.reloptions IS NOT NULL) UNION ALL SELECT 1 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE " USER_NAMESPACE " UNION ALL SELECT 1 FROM pg_catalog.pg_type t JOIN pg_catalog.pg_namespace n ON n.oid=t.typnamespace WHERE " USER_NAMESPACE " AND t.typtype<>'c' AND t.typelem=0", 0, NULL, NULL, NULL, true);
	if (SPI_processed)
		unsupported("database capture containing non-prolly relations, routines, custom types or unsupported relation properties");
	run("SELECT 1 FROM pg_collation c JOIN pg_namespace n ON n.oid=c.collnamespace WHERE " USER_NAMESPACE " UNION ALL SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid=o.oprnamespace WHERE " USER_NAMESPACE, 0, NULL, NULL, NULL, true);
	if (SPI_processed)
		unsupported("capture of custom collations or operators");
	run("SELECT 1 FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid=a.attrelid JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace JOIN pg_catalog.pg_type t ON t.oid=a.atttypid WHERE " USER_NAMESPACE " AND c.relkind='r' AND c.relpersistence<>'t' AND a.attnum>0 AND (a.attisdropped OR a.attacl IS NOT NULL OR a.attoptions IS NOT NULL OR a.atthasdef OR a.attidentity<>'' OR a.attgenerated<>'' OR t.typnamespace<>'pg_catalog'::regnamespace) UNION ALL SELECT 1 FROM pg_catalog.pg_trigger t JOIN pg_catalog.pg_class c ON c.oid=t.tgrelid JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE " USER_NAMESPACE " AND c.relpersistence<>'t' UNION ALL SELECT 1 FROM pg_catalog.pg_constraint x JOIN pg_catalog.pg_namespace n ON n.oid=x.connamespace WHERE " USER_NAMESPACE " AND x.contype NOT IN ('p','u','n')", 0, NULL, NULL, NULL, true);
	if (SPI_processed)
		unsupported("capture of defaults, generated/dropped columns, user types, triggers or non-unique constraints");
	run("SELECT jsonb_build_object('format','DGNS1','name',n.nspname,'owner',pg_get_userbyid(n.nspowner),'comment',obj_description(n.oid,'pg_namespace'),'acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode(COALESCE(n.nspacl,acldefault('n',n.nspowner))) x),'[]'::jsonb))::text FROM pg_catalog.pg_namespace n WHERE " USER_NAMESPACE " ORDER BY n.nspname COLLATE \"C\"", 0, NULL, NULL, NULL, true);
	nns = SPI_processed;
	namespaces = palloc(sizeof(char *) * Max(nns, 1));
	for (i = 0; i < nns; i++)
		namespaces[i] = strcell(i, 1);
	members = palloc(sizeof(Datum) * Max(nns + nrel, 1));
	initStringInfo(&json);
	appendStringInfoString(&json, "{\"format\":\"DGDB1\",\"members\":[");
	for (i = 0; i < nns; i++)
	{
		if (used)
			appendStringInfoChar(&json, ',');
		appendStringInfo(&json, "{\"kind\":\"namespace\",\"definition\":%s}", namespaces[i]);
		if (persist)
			members[used] = PointerGetDatum(put("namespace", namespaces[i], NULL, 0));
		used++;
	}
	for (i = 0; i < nrel; i++)
	{
		Oid			types[] = {OIDOID};
		Datum		args[] = {ObjectIdGetDatum(relations[i])};
		char	   *name,
				   *kind,
				   *schema,
				   *schemahash,
				   *content;
		bytea	   *chunk = NULL,
				   *desc = NULL,
				   *shash = NULL;
		StringInfoData member;
		DGState    *state;
		Relation	rel;

		run("SELECT format('%I.%I',n.nspname,c.relname),CASE WHEN c.relkind='r' THEN 'table' ELSE 'index' END FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE c.oid=$1", 1, types, args, NULL, true);
		name = strcell(0, 1);
		kind = strcell(0, 2);
		run("SELECT jsonb_build_object('format','DGTABLE1','name',format('%I.%I',n.nspname,c.relname),'owner',pg_get_userbyid(c.relowner),'comment',obj_description(c.oid,'pg_class'),'acl',COALESCE((SELECT jsonb_agg(jsonb_build_array(pg_get_userbyid(x.grantor),CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END,x.privilege_type,x.is_grantable) ORDER BY pg_get_userbyid(x.grantor) COLLATE \"C\",CASE WHEN x.grantee=0 THEN 'PUBLIC' ELSE pg_get_userbyid(x.grantee) END COLLATE \"C\",x.privilege_type COLLATE \"C\") FROM aclexplode(COALESCE(c.relacl,acldefault('r',c.relowner))) x),'[]'::jsonb),'columns',COALESCE((SELECT jsonb_agg(jsonb_build_object('name',a.attname,'comment',col_description(c.oid,a.attnum),'type',t.typname,'typmod',a.atttypmod,'notnull',a.attnotnull,'collation',CASE WHEN a.attcollation=0 THEN NULL ELSE (SELECT jsonb_build_array(cn.nspname,co.collname,co.collprovider,co.collisdeterministic,co.collcollate,co.collctype,co.colllocale,co.collversion) FROM pg_collation co JOIN pg_namespace cn ON cn.oid=co.collnamespace WHERE co.oid=a.attcollation) END) ORDER BY a.attnum) FROM pg_attribute a JOIN pg_type t ON t.oid=a.atttypid WHERE a.attrelid=c.oid AND a.attnum>0),'[]'::jsonb),'indexes',COALESCE((SELECT jsonb_agg(pg_get_indexdef(x.indexrelid) ORDER BY ic.relname COLLATE \"C\") FROM pg_index x JOIN pg_class ic ON ic.oid=x.indexrelid WHERE x.indrelid=c.oid),'[]'::jsonb),'constraints',COALESCE((SELECT jsonb_agg(jsonb_build_array(conname,pg_get_constraintdef(oid)) ORDER BY conname COLLATE \"C\") FROM pg_constraint WHERE conrelid=c.oid),'[]'::jsonb),'index_definition',CASE WHEN c.relkind='i' THEN pg_get_indexdef(c.oid) ELSE NULL END)::text FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE c.oid=$1", 1, types, args, NULL, true);
		schema = strcell(0, 1);
		run("SELECT encode(sha256(convert_to($1,'UTF8')),'hex')", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(schema)), NULL, true);
		schemahash = strcell(0, 1);
		rel = relation_open(relations[i], NoLock);
		if (all_catalogs)
			content = pstrdup("");	/* Definition validation never reads
									 * another branch's rows. */
		else
		{
			state = dg_load(rel, GetActiveSnapshot());
			{
				bytea	   *raw = palloc(VARHDRSZ + 32);

				SET_VARSIZE(raw, VARHDRSZ + 32);
				memcpy(VARDATA(raw), state->content.root.bytes, 32);
				content = hex(raw);
			}
			dg_release(state);
		}
		relation_close(rel, NoLock);
		initStringInfo(&member);
		appendStringInfoChar(&member, '{');
		json_field(&member, "kind", kind, false);
		json_field(&member, "name", name, true);
		json_field(&member, "schema", schemahash, true);
		json_field(&member, "content", content, true);
		appendStringInfoChar(&member, '}');
		if (persist)
		{
			shash = put("schema", schema, NULL, 0);
			chunk = DatumGetByteaP(DirectFunctionCall1(dolt_manifest_export, ObjectIdGetDatum(relations[i])));
			desc = put(kind, member.data, VC_ARGS(PointerGetDatum(shash), PointerGetDatum(chunk)), 2);
			members[used] = PointerGetDatum(desc);
		}
		if (used)
			appendStringInfoChar(&json, ',');
		appendStringInfoString(&json, member.data);
		used++;
	}
	appendStringInfoString(&json, "]}");
	/* Sort members by symbolic identity, never relation OID / creation order. */
	run("SELECT jsonb_build_object('format','DGDB1','encoding',current_setting('server_encoding'),'members',COALESCE(jsonb_agg(v ORDER BY v->>'kind' COLLATE \"C\",COALESCE(v->>'name',v->'definition'->>'name') COLLATE \"C\"),'[]'::jsonb))::text FROM jsonb_array_elements($1::jsonb->'members') v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(json.data)), NULL, true);
	result.json = strcell(0, 1);
	if (persist)
	{
		/*
		 * Hash-sort child addresses as a canonical set; payload carries
		 * membership names.
		 */
		run("SELECT coalesce(array_agg(h ORDER BY h),'{}'::bytea[]) FROM unnest($1::bytea[]) h", 1, VC_TYPES(BYTEAARRAYOID), VC_ARGS(PointerGetDatum(links(members, used))), NULL, true);
		{
			ArrayType  *arr = DatumGetArrayTypeP(cell(0, 1));
			Datum	   *vals;
			bool	   *nulls;
			int			n;

			deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT, &vals, &nulls, &n);
			result.hash = put("database", result.json, vals, n);
		}
	}
	PopActiveSnapshot();

	/*
	 * Every historical byte has been copied; no source locator escapes
	 * capture.
	 */
	for (i = nrel - 1; i >= 0; i--)
		UnlockRelationOid(relations[i], AccessShareLock);
	for (i = lengthof(catalogs) - 1; i >= 0; i--)
		UnlockRelationOid(catalogs[i], ShareLock);
	return result;
}
static Capture
capture_mode(bool persist, bool all_catalogs)
{
	Capture		result;
	int			nest = NewGUCNestLevel();

	set_config_option("search_path", "pg_catalog", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("quote_all_identifiers", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	PG_TRY();
	{
		result = capture_inner(persist, all_catalogs);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return result;
}

static Capture
capture(bool persist)
{
	return capture_mode(persist, false);
}

static char *
json_value(const char *json, const char *key)
{
	run("SELECT $1::jsonb->>$2", 2, VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(json), CStringGetTextDatum(key)), NULL, true);
	return strcell(0, 1);
}
static bytea *
commit_object(bytea *database, bytea *parent, bytea *second, const char *message, const char *author, const char *author_email, TimestampTz author_date)
{
	StringInfoData json;
	const char *name = GetUserNameFromId(GetUserId(), false);
	char	   *email = psprintf("%s@localhost", name);
	int64		height = 1;

	if (parent)
	{
		char	   *meta = metadata(parent, "commit");
		char	   *h = json_value(meta, "height");

		if (!h || strcmp(json_value(meta, "format"), "DGCOMMIT1"))
			unsupported("commit metadata from an incompatible internal format");
		height = pg_strtoint64(h) + 1;
	}
	if (second)
		height = Max(height, pg_strtoint64(json_value(metadata(second, "commit"), "height")) + 1);
	initStringInfo(&json);
	appendStringInfoChar(&json, '{');
	json_field(&json, "format", "DGCOMMIT1", false);
	json_field(&json, "committer", name, true);
	json_field(&json, "email", email, true);
	json_field(&json, "author", author ? author : name, true);
	json_field(&json, "author_email", author_email ? author_email : email, true);
	json_field(&json, "message", message, true);
	appendStringInfo(&json, ",\"date_us\":%lld,\"author_date_us\":%lld,\"height\":%lld}", (long long) GetCurrentTimestamp(), (long long) author_date, (long long) height);
	return put("commit", json.data, VC_ARGS(PointerGetDatum(database), PointerGetDatum(parent), PointerGetDatum(second)), second ? 3 : parent ? 2 : 1);
}
static void
ensure_repository(void)
{
	VersionRef	head = read_ref("heads/main");
	bytea	   *db,
			   *commit;

	if (head.target)
		return;
	db = put("database", "{\"format\":\"DGDB1\",\"members\":[]}", NULL, 0);
	commit = commit_object(db, NULL, NULL, "Initialize data repository", NULL, NULL, GetCurrentTimestamp());
	move_ref("heads/main", head, commit);
	move_ref("staged/main", read_ref("staged/main"), db);
	move_ref("working/main", read_ref("working/main"), db);
	CommandCounterIncrement();
	/* Our command's active snapshot must see repository initialization. */
	UpdateActiveSnapshotCommandId();
}
static void
valid_name(const char *s)
{
	const char *p;

	if (!*s || strlen(s) > 200 || !pg_strcasecmp(s, "HEAD") || !pg_strcasecmp(s, "WORKING") || !pg_strcasecmp(s, "STAGED") || strstr(s, "..") || strstr(s, "@{") || s[0] == '-' || s[0] == '/' || s[strlen(s) - 1] == '/' || s[strlen(s) - 1] == '.' || strstr(s, "//") || (strlen(s) >= 5 && !strcmp(s + strlen(s) - 5, ".lock")))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid branch or tag name: %s", s)));
	for (p = s; *p; p++)
		if ((unsigned char) *p <= 32 || (unsigned char) *p == 127 || strchr("~^:?*[\\", *p))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid branch or tag name: %s", s)));
}
static bytea *
resolve(const char *revision)
{
	char	   *base = pstrdup(revision),
			   *suffix = strpbrk(base, "~^");
	bytea	   *hash = NULL;
	VersionRef	ref;

	if (suffix)
		*suffix++ = '\0';
	if (!pg_strcasecmp(base, "HEAD"))
		hash = read_ref(branch_ref("heads")).target;
	else
	{
		ref = read_ref(psprintf("heads/%s", base));
		hash = ref.target;
		if (!hash)
		{
			ref = read_ref(psprintf("tags/%s", base));
			hash = ref.target;
			if (hash)
			{
				run("SELECT objkind FROM pg_dolt_object WHERE objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(hash)), NULL, true);
				if (SPI_processed && !strcmp(strcell(0, 1), "tag"))
					hash = child(hash, 1);
			}
		}
		if (!hash && strlen(base) == 64)
			hash = unhex(base);
		else if (!hash && strlen(base) >= 8 && strlen(base) < 64 &&
				 strspn(base, "0123456789abcdef") == strlen(base))
		{
			run("SELECT objhash FROM pg_dolt_object WHERE objkind='commit' AND encode(objhash,'hex') LIKE $1 LIMIT 2", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(psprintf("%s%%", base))), NULL, true);
			if (SPI_processed > 1)
				ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_PARAMETER), errmsg("ambiguous commit prefix: %s", base)));
			if (SPI_processed)
				hash = DatumGetByteaPCopy(cell(0, 1));
		}
	}
	if (!hash)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("unknown revision: %s", revision)));
	metadata(hash, "commit");
	if (suffix)
	{
		const char *orig = strpbrk(revision, "~^");
		int			n = *suffix ? pg_strtoint32(suffix) : 1,
					i;

		if (n < 0 || n > 100000)
			unsupported("ancestor count outside supported range");
		if (*orig == '^')
		{
			if (n)
				hash = child(hash, n + 1);
		}
		else
			for (i = 0; i < n; i++)
			{
				CHECK_FOR_INTERRUPTS();
				hash = child(hash, 2);
			}
	}
	return hash;
}
static void
author_option(char *value, char **name, char **email)
{
	char	   *lt = strrchr(value, '<'),
			   *gt = strrchr(value, '>');

	if (!lt || !gt || gt[1] || gt < lt + 2 || lt == value)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("author must be Name <email>")));
	*name = pnstrdup(value, lt - value);
	while (**name && (*name)[strlen(*name) - 1] == ' ')
		(*name)[strlen(*name) - 1] = '\0';
	*email = pnstrdup(lt + 1, gt - lt - 1);
}

Datum
dolt_add(PG_FUNCTION_ARGS)
{
	int			n;
	char	  **args = arguments(fcinfo, &n);
	Capture		current;
	VersionRef	staged,
				working;
	int			nest;

	standalone(fcinfo, false);
	nest = begin_api();
	if (n != 1 || (strcmp(args[0], ".") && strcmp(args[0], "-A") && strcmp(args[0], "--all")))
		unsupported("dolt_add currently requires '.', '-A' or '--all'");
	ensure_repository();
	staged = read_ref(branch_ref("staged"));
	working = read_ref(branch_ref("working"));
	current = capture(true);
	move_ref(branch_ref("staged"), staged, current.hash);
	move_ref(branch_ref("working"), working, current.hash);
	end_api(nest);
	PG_RETURN_INT64(0);
}
Datum
dolt_commit(PG_FUNCTION_ARGS)
{
	int			n,
				i;
	char	  **args = arguments(fcinfo, &n),
			   *message = NULL,
			   *author = NULL,
			   *email = NULL;
	bool		all = false,
				allow_empty = false,
				skip_empty = false;
	TimestampTz author_date = GetCurrentTimestamp();
	VersionRef	head,
				staged,
				working;
	bytea	   *database,
			   *parentdb,
			   *commit;
	char	   *answer;
	int			nest;

	standalone(fcinfo, true);
	nest = begin_api();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-A") || !strcmp(args[i], "--all"))
			all = true;
		else if (!strcmp(args[i], "--allow-empty"))
			allow_empty = true;
		else if (!strcmp(args[i], "--skip-empty"))
			skip_empty = true;
		else if (!strcmp(args[i], "-m") || !strcmp(args[i], "--message") || (!strcmp(args[i], "-Am") || !strcmp(args[i], "-am")))
		{
			if ((!strcmp(args[i], "-Am") || !strcmp(args[i], "-am")))
				all = true;
			if (++i == n)
				unsupported("missing commit message");
			message = args[i];
		}
		else if (!strcmp(args[i], "--author"))
		{
			if (++i == n)
				unsupported("missing author");
			author_option(args[i], &author, &email);
		}
		else if (!strcmp(args[i], "--date"))
		{
			if (++i == n)
				unsupported("missing date");
			author_date = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in, CStringGetDatum(args[i]), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
		}
		else
			unsupported(args[i]);
	}
	if (!message || !*message)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("commit message is required (-m)")));
	if (allow_empty && skip_empty)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("--allow-empty and --skip-empty are incompatible")));
	ensure_repository();
	head = read_ref(branch_ref("heads"));
	staged = read_ref(branch_ref("staged"));
	working = read_ref(branch_ref("working"));
	parentdb = child(head.target, 1);
	database = all ? capture(true).hash : staged.target;
	if (!database)
		database = parentdb;
	if (same(database, parentdb) && !allow_empty && !merge_parent())
	{
		if (skip_empty)
		{
			end_api(nest);
			PG_RETURN_NULL();
		}
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("nothing to commit")));
	}
	commit = commit_object(database, head.target, merge_parent(), message, author, email, author_date);
	move_ref(branch_ref("heads"), head, commit);
	move_ref(branch_ref("staged"), staged, database);
	if (all)
		move_ref(branch_ref("working"), working, database);
	finish_merge();
	answer = hex(commit);
	end_api(nest);
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}
Datum
dolt_branch(PG_FUNCTION_ARGS)
{
	int			n;
	char	  **args = arguments(fcinfo, &n);
	VersionRef	ref;
	bytea	   *commit;
	int			nest;

	standalone(fcinfo, true);
	nest = begin_api();
	if (n < 1 || n > 2 || args[0][0] == '-')
		unsupported("dolt_branch supports creation: name [start-point]");
	valid_name(args[0]);
	ensure_repository();
	commit = resolve(n == 2 ? args[1] : "HEAD");
	ref = read_ref(psprintf("heads/%s", args[0]));
	if (ref.target)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("branch already exists: %s", args[0])));
	move_ref(psprintf("heads/%s", args[0]), ref, commit);
	move_ref(psprintf("working/%s", args[0]), empty_ref, child(commit, 1));
	move_ref(psprintf("staged/%s", args[0]), empty_ref, child(commit, 1));
	end_api(nest);
	PG_RETURN_INT64(0);
}
Datum
dolt_tag(PG_FUNCTION_ARGS)
{
	int			n,
				i,
				pos = 0;
	char	  **args = arguments(fcinfo, &n),
			   *names[2],
			   *message = "",
			   *author = NULL,
			   *email = NULL;
	bytea	   *commit,
			   *tag;
	VersionRef	ref;
	StringInfoData json;
	int			nest;

	standalone(fcinfo, true);
	nest = begin_api();
	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "-m") || !strcmp(args[i], "--message"))
		{
			if (++i == n)
				unsupported("missing tag message");
			message = args[i];
		}
		else if (!strcmp(args[i], "--author"))
		{
			if (++i == n)
				unsupported("missing tag author");
			author_option(args[i], &author, &email);
		}
		else if (args[i][0] == '-' || pos == 2)
			unsupported(args[i]);
		else
			names[pos++] = args[i];
	}
	if (pos == 0)
		unsupported("tag listing: query dolt_tags instead");
	valid_name(names[0]);
	ensure_repository();
	commit = resolve(pos == 2 ? names[1] : "HEAD");
	ref = read_ref(psprintf("tags/%s", names[0]));
	if (ref.target)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("tag already exists: %s", names[0])));
	if (!author)
	{
		author = GetUserNameFromId(GetUserId(), false);
		email = psprintf("%s@localhost", author);
	}
	initStringInfo(&json);
	appendStringInfoChar(&json, '{');
	json_field(&json, "format", "DGTAG1", false);
	json_field(&json, "tagger", author, true);
	json_field(&json, "email", email, true);
	json_field(&json, "message", message, true);
	appendStringInfo(&json, ",\"date_us\":%lld}", (long long) GetCurrentTimestamp());
	tag = put("tag", json.data, VC_ARGS(PointerGetDatum(commit)), 1);
	move_ref(psprintf("tags/%s", names[0]), ref, tag);
	end_api(nest);
	PG_RETURN_INT64(0);
}
static void
return_rows(FunctionCallInfo fcinfo, int nest)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	uint64		r;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	for (r = 0; r < SPI_processed; r++)
	{
		Datum		values[16];
		bool		nulls[16];
		int			c;

		for (c = 0; c < SPI_tuptable->tupdesc->natts; c++)
			values[c] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c + 1, &nulls[c]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	end_api(nest);
}
Datum
dolt_log(PG_FUNCTION_ARGS)
{
	int			n,
				i,
				limit = -1;
	char	  **args = arguments(fcinfo, &n),
			   *revision = "HEAD";
	bool		parents = false,
				signature = false,
				revset = false;
	bytea	   *hash;
	int			nest = begin_api();

	for (i = 0; i < n; i++)
	{
		if (!strcmp(args[i], "--parents"))
			parents = true;
		else if (!strcmp(args[i], "--show-signature"))
			signature = true;
		else if (!strcmp(args[i], "-n") || !strcmp(args[i], "--max-count"))
		{
			if (++i == n)
				unsupported("missing log count");
			limit = pg_strtoint32(args[i]);
			if (limit < 0)
				unsupported("negative log count");
		}
		else if (args[i][0] == '-' || revset)
			unsupported(args[i]);
		else
		{
			revision = args[i];
			revset = true;
		}
	}
	if (!read_ref(branch_ref("heads")).target && !strcmp(revision, "HEAD"))
	{
		run("SELECT NULL::text WHERE false", 0, NULL, NULL, NULL, true);
		return_rows(fcinfo, nest);
		return (Datum) 0;
	}
	hash = resolve(revision);
	run("WITH RECURSIVE walk(h) AS (SELECT $1::bytea UNION SELECT unnest(o.objchildren[2:]) FROM walk w JOIN pg_dolt_object o ON o.objhash=w.h), commits AS (SELECT o.*,convert_from(o.objpayload,'UTF8')::jsonb m FROM walk w JOIN pg_dolt_object o ON o.objhash=w.h WHERE o.objkind='commit'), refs AS (SELECT r.refname,CASE WHEN o.objkind='tag' THEN o.objchildren[1] ELSE r.reftarget END target FROM pg_dolt_ref r JOIN pg_dolt_object o ON o.objhash=r.reftarget WHERE r.refname LIKE 'heads/%' OR r.refname LIKE 'tags/%') SELECT encode(c.objhash,'hex'),m->>'committer',m->>'email',timestamp '2000-01-01' + (m->>'date_us')::bigint * interval '1 microsecond',m->>'message',(m->>'height')::bigint,CASE WHEN $2 THEN COALESCE((SELECT string_agg(encode(h,'hex'),', ' ORDER BY ord) FROM unnest(c.objchildren[2:]) WITH ORDINALITY u(h,ord)),'') ELSE NULL END,COALESCE((SELECT CASE WHEN c.objhash=(SELECT reftarget FROM pg_dolt_ref WHERE refname='heads/' || pg_catalog.active_branch()) THEN 'HEAD -> ' ELSE '' END || string_agg(CASE WHEN r.refname LIKE 'tags/%' THEN 'tag: '||substr(r.refname,6) ELSE substr(r.refname,7) END,', ' ORDER BY r.refname COLLATE \"C\") FROM refs r WHERE r.target=c.objhash),''),CASE WHEN $3 THEN ''::text ELSE NULL END,m->>'author',m->>'author_email',timestamp '2000-01-01' + (m->>'author_date_us')::bigint * interval '1 microsecond' FROM commits c ORDER BY (m->>'height')::bigint DESC,(m->>'date_us')::bigint DESC,c.objhash LIMIT NULLIF($4,-1)", 4, VC_TYPES(BYTEAOID, BOOLOID, BOOLOID, INT4OID), VC_ARGS(PointerGetDatum(hash), BoolGetDatum(parents), BoolGetDatum(signature), Int32GetDatum(limit)), NULL, true);
	return_rows(fcinfo, nest);
	return (Datum) 0;
}
static char *
state_json(const char *revision)
{
	if (!pg_strcasecmp(revision, "WORKING"))
		return capture(false).json;
	if (!pg_strcasecmp(revision, "STAGED"))
	{
		VersionRef	r = read_ref(branch_ref("staged"));

		if (!r.target)
			return "{\"members\":[]}";
		return metadata(r.target, "database");
	}
	return metadata(child(resolve(revision), 1), "database");
}
Datum
dolt_diff_summary(PG_FUNCTION_ARGS)
{
	char	   *from,
			   *to,
			   *table = NULL;
	int			nest = begin_api();

	from = state_json(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	to = !strcmp(text_to_cstring(PG_GETARG_TEXT_PP(0)), text_to_cstring(PG_GETARG_TEXT_PP(1))) ? from : state_json(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	if (PG_NARGS() > 2)
		table = text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (table)
	{
		run("WITH members AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v UNION ALL SELECT v FROM jsonb_array_elements($2::jsonb->'members') v) SELECT DISTINCT v->>'name' FROM members WHERE v->>'kind'='table' AND (v->>'name'=$3 OR (array_length(parse_ident($3),1)=1 AND (parse_ident(v->>'name'))[2]=(parse_ident($3))[1]))", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(from), CStringGetTextDatum(to), CStringGetTextDatum(table)), NULL, true);
		if (!SPI_processed)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("table does not exist in either revision: %s", table)));
		if (SPI_processed > 1)
			ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous table name: %s", table)));
		table = strcell(0, 1);
	}

	run("WITH a AS (SELECT v FROM jsonb_array_elements($1::jsonb->'members') v WHERE v->>'kind'='table'),b AS (SELECT v FROM jsonb_array_elements($2::jsonb->'members') v WHERE v->>'kind'='table'),d AS (SELECT a.v av,b.v bv FROM a FULL JOIN b ON a.v->>'name'=b.v->>'name') SELECT coalesce(av->>'name',''),coalesce(bv->>'name',''),CASE WHEN av IS NULL THEN 'added' WHEN bv IS NULL THEN 'dropped' ELSE 'modified' END,(av->>'content') IS DISTINCT FROM (bv->>'content'),(av->>'schema') IS DISTINCT FROM (bv->>'schema') FROM d WHERE ((av->>'content') IS DISTINCT FROM (bv->>'content') OR (av->>'schema') IS DISTINCT FROM (bv->>'schema')) AND ($3::text IS NULL OR coalesce(av->>'name',bv->>'name')=$3 OR (array_length(parse_ident($3),1)=1 AND (parse_ident(coalesce(av->>'name',bv->>'name')))[2]=(parse_ident($3))[1])) ORDER BY coalesce(av->>'name',bv->>'name') COLLATE \"C\"", 3, VC_TYPES(TEXTOID, TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(from), CStringGetTextDatum(to), table ? CStringGetTextDatum(table) : (Datum) 0), table ? NULL : "  n", true);
	if (table && SPI_processed > 1)
		ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_ALIAS), errmsg("ambiguous table name: %s", table)));
	return_rows(fcinfo, nest);
	return (Datum) 0;
}

/* Distinct fmgr entries keep overload arity explicit in the built-in catalog. */

Datum
dolt_add_noargs(PG_FUNCTION_ARGS)
{
	return dolt_add(fcinfo);
}

Datum
dolt_commit_noargs(PG_FUNCTION_ARGS)
{
	return dolt_commit(fcinfo);
}

Datum
dolt_branch_noargs(PG_FUNCTION_ARGS)
{
	return dolt_branch(fcinfo);
}

Datum
dolt_tag_noargs(PG_FUNCTION_ARGS)
{
	return dolt_tag(fcinfo);
}

Datum
dolt_log_noargs(PG_FUNCTION_ARGS)
{
	return dolt_log(fcinfo);
}

Datum
dolt_diff_summary_table(PG_FUNCTION_ARGS)
{
	return dolt_diff_summary(fcinfo);
}

#include "dg_merge.inc"



#include "dg_catalog.inc"

#include "dg_catalog_ddl.inc"
