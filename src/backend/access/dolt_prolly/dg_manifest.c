/* Heap/MVCC manifest building blocks. See doc/doltgres/manifest.md. */
#include "dg.h"
#include "catalog/pg_dolt_object.h"
#include "catalog/pg_dolt_ref.h"
#include "utils/inval.h"
#include "access/relation.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/snapmgr.h"
#include "utils/json.h"
#include "mb/pg_wchar.h"

#define DG_SCHEMA_TREE "DGSCHEMA2"

static const char *const kinds[] = {"chunk", "schema", "table", "index", "database", "commit", "namespace", "tag", "merge"};

static void
manifest_begin(void)
{
	MemoryContext caller = CurrentMemoryContext;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("manifest foundation functions require superuser")));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "cannot connect manifest SPI");
	MemoryContextSwitchTo(caller);
}

/* SPI restores its procedure context, not an arbitrary caller context. */
static void
manifest_execute(const char *sql, int nargs, Oid *types, Datum *args,
				 const char *nulls, bool readonly, long count)
{
	MemoryContext caller = CurrentMemoryContext;

	SPI_execute_with_args(sql, nargs, types, args, nulls, readonly, count);
	MemoryContextSwitchTo(caller);
}

static bytea *
bytes(const void *p, Size n)
{
	bytea	   *v = palloc(VARHDRSZ + n);

	SET_VARSIZE(v, VARHDRSZ + n);
	if (n)
		memcpy(VARDATA(v), p, n);
	return v;
}

static void
check_hash(bytea *hash)
{
	if (VARSIZE_ANY_EXHDR(hash) != PL_HASH_SIZE)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest hashes and revision tokens must be 32 bytes")));
}

static int
kind_id(const char *kind)
{
	int			i;

	for (i = 0; i < lengthof(kinds); i++)
		if (strcmp(kind, kinds[i]) == 0)
			return i;
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown manifest object kind: %s", kind)));
	return -1;
}

static char *
object_kind(bytea *hash, bool readonly)
{
	Oid			types[] = {BYTEAOID};
	Datum		args[] = {PointerGetDatum(hash)};
	char	   *kind;

	check_hash(hash);
	manifest_execute("SELECT objkind FROM pg_catalog.pg_dolt_object_visible WHERE objhash=$1", 1, types, args, NULL, readonly, 1);
	if (SPI_processed != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest target object is not visible")));
	kind = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
	return pstrdup(kind);
}

static ArrayType *
children_array(Datum *values, int count)
{
	if (!count)
		return construct_empty_array(BYTEAOID);
	return construct_array(values, count, BYTEAOID, -1, false, TYPALIGN_INT);
}

/* Children are immutable and already visible, so construction cannot form cycles. */
static bytea *
put_object(const char *kind, bytea *payload, ArrayType *children)
{
	int			id = kind_id(kind),
				n,
				i;
	Datum	   *links;
	bool	   *nulls;
	Size		length;
	uint8	   *encoding,
			   *p;
	PLHash		hash;
	PLSlice		input;
	bytea	   *address;
	Oid			types[] = {BYTEAOID, TEXTOID, BYTEAOID, BYTEAARRAYOID};
	Datum		args[4];

	if (ARR_NDIM(children) > 1 || (ARR_NDIM(children) == 1 && ARR_LBOUND(children)[0] != 1))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest children must be a one-dimensional array with lower bound 1")));
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &links, &nulls, &n);
	if (((id == 1 || id == 6) && n > 1) || ((id == 2 || id == 3) && n != 2) || (id == 5 && n < 1) || (id == 7 && n != 1) || (id == 8 && n != 3))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid manifest child count for %s", kind)));
	if ((id == 1 || id == 6) && n == 1 &&
		(VARSIZE_ANY_EXHDR(payload) != strlen(DG_SCHEMA_TREE) ||
		 memcmp(VARDATA_ANY(payload), DG_SCHEMA_TREE, strlen(DG_SCHEMA_TREE))))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid schema tree envelope")));
	for (i = 0; i < n; i++)
	{
		char	   *child;
		bool		valid;

		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("manifest child cannot be null")));
		child = object_kind(DatumGetByteaPP(links[i]), false);
		valid = (id == 0 || id == 1 || id == 6) ? strcmp(child, "chunk") == 0 :
			(id == 2 || id == 3) ? strcmp(child, i == 0 ? "schema" : "chunk") == 0 :
			id == 4 ? (strcmp(child, "table") == 0 || strcmp(child, "index") == 0 || strcmp(child, "namespace") == 0) :
			id == 5 ? strcmp(child, i == 0 ? "database" : "commit") == 0 :
			(id == 7 || id == 8) ? strcmp(child, "commit") == 0 : false;
		if (!valid)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid %s child for manifest %s", child, kind)));
	}
	length = 20 + (Size) n * PL_HASH_SIZE + VARSIZE_ANY_EXHDR(payload);
	if (length > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("manifest object too large")));
	encoding = palloc(length);
	memcpy(encoding, "DGMOBJ01", 8);
	pl_write32(encoding + 8, id);
	pl_write32(encoding + 12, n);
	pl_write32(encoding + 16, VARSIZE_ANY_EXHDR(payload));
	p = encoding + 20;
	for (i = 0; i < n; i++, p += PL_HASH_SIZE)
		memcpy(p, VARDATA_ANY(DatumGetByteaPP(links[i])), PL_HASH_SIZE);
	memcpy(p, VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload));
	input.data = encoding;
	input.len = length;
	if (!pl_hash_bytes(input, &hash))
		elog(ERROR, "manifest SHA-256 failed");
	address = bytes(hash.bytes, PL_HASH_SIZE);
	args[0] = PointerGetDatum(address);
	args[1] = CStringGetTextDatum(kind);
	args[2] = PointerGetDatum(payload);
	args[3] = PointerGetDatum(children);

	/*
	 * Identical immutable objects can be inserted by independent
	 * transactions. Their snapshot-visible logical representation is
	 * deduplicated by the view.
	 */
	manifest_execute("SELECT objkind=$2 AND objpayload=$3 AND objchildren=$4 AS same FROM pg_catalog.pg_dolt_object_visible WHERE objhash=$1", 4, types, args, NULL, false, 1);
	if (SPI_processed)
	{
		bool		isnull;
		Datum		same = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

		if (isnull || !DatumGetBool(same))
			elog(ERROR, "manifest object hash collision");
	}
	else
		manifest_execute("INSERT INTO pg_catalog.pg_dolt_object(objhash,objkind,objpayload,objchildren,objid) VALUES($1,$2,$3,$4,gen_random_uuid())", 4, types, args, NULL, false, 0);
	pfree(encoding);
	return address;
}

Datum
dolt_manifest_put(PG_FUNCTION_ARGS)
{
	bytea	   *result;

	manifest_begin();
	result = put_object(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_BYTEA_PP(1), PG_GETARG_ARRAYTYPE_P(2));
	SPI_finish();
	PG_RETURN_BYTEA_P(result);
}

static Datum
record_result(FunctionCallInfo fcinfo, int columns)
{
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3];
	HeapTuple	result;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "manifest function requires composite result");
	for (i = 0; i < columns; i++)
	{
		nulls[i] = true;
		values[i] = (Datum) 0;
		if (SPI_processed)
		{
			values[i] = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, i + 1, &nulls[i]);
			if (!nulls[i])
				values[i] = datumCopy(values[i], false, -1);
		}
	}
	result = heap_form_tuple(BlessTupleDesc(desc), values, nulls);
	SPI_finish();
	return HeapTupleGetDatum(result);
}

Datum
dolt_manifest_get(PG_FUNCTION_ARGS)
{
	Oid			types[] = {BYTEAOID};
	Datum		args[] = {PG_GETARG_DATUM(0)};

	manifest_begin();
	check_hash(PG_GETARG_BYTEA_PP(0));
	manifest_execute("SELECT objkind,objpayload,objchildren FROM pg_catalog.pg_dolt_object_visible WHERE objhash=$1", 1, types, args, NULL, true, 1);
	return record_result(fcinfo, 3);
}

static void
check_name(text *name)
{
	char	   *s = text_to_cstring(name);
	int			prefix = strncmp(s, "merge/", 6) == 0 ? 6 : strncmp(s, "views/", 6) == 0 ? 6 : strncmp(s, "heads/", 6) == 0 ? 6 : strncmp(s, "tags/", 5) == 0 ? 5 : strncmp(s, "working/", 8) == 0 ? 8 : strncmp(s, "staged/", 7) == 0 ? 7 : 0;

	if (!prefix || strlen(s) == prefix || strlen(s) > 512)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest ref requires heads/, tags/, staged/, working/, merge/ or views/ and a nonempty name (maximum 512 bytes)")));
}

Datum
dolt_manifest_read(PG_FUNCTION_ARGS)
{
	Oid			types[] = {TEXTOID};
	Datum		args[] = {PG_GETARG_DATUM(0)};

	manifest_begin();
	check_name(PG_GETARG_TEXT_PP(0));
	manifest_execute("SELECT reftarget,reftoken FROM pg_catalog.pg_dolt_ref WHERE refname=$1", 1, types, args, NULL, true, 1);
	return record_result(fcinfo, 2);
}

Datum
dolt_manifest_cas(PG_FUNCTION_ARGS)
{
	Oid			types[] = {TEXTOID, BYTEAOID, BYTEAOID, BYTEAOID};
	Datum		args[4];
	char		nulls[] = "    ";
	uint8		random[PL_HASH_SIZE];
	bytea	   *token;
	text	   *name;

	manifest_begin();
	if (PG_ARGISNULL(0) || (PG_ARGISNULL(1) && PG_ARGISNULL(2)))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("manifest CAS needs a name and expected token or target")));
	name = PG_GETARG_TEXT_PP(0);
	check_name(name);
	args[0] = PointerGetDatum(name);
	args[1] = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);
	args[2] = PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2);
	if (PG_ARGISNULL(1))
		nulls[1] = 'n';
	else
		check_hash(PG_GETARG_BYTEA_PP(1));
	if (PG_ARGISNULL(2))
		nulls[2] = 'n';
	else
	{
		char	   *kind = object_kind(PG_GETARG_BYTEA_PP(2), false);
		char	   *expected = strncmp(text_to_cstring(name), "merge/", 6) == 0 ? "merge" : (strncmp(text_to_cstring(name), "working/", 8) == 0 || strncmp(text_to_cstring(name), "staged/", 7) == 0) ? "database" : "commit";

		if (strcmp(kind, expected) != 0 &&
			!(strncmp(text_to_cstring(name), "tags/", 5) == 0 && strcmp(kind, "tag") == 0))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("manifest ref requires %s target", expected)));
	}
	if (!pg_strong_random(random, sizeof(random)))
		elog(ERROR, "cannot generate manifest revision token");
	token = bytes(random, sizeof(random));
	args[3] = PointerGetDatum(token);
	if (PG_ARGISNULL(1))
		manifest_execute("INSERT INTO pg_catalog.pg_dolt_ref VALUES($1,$3,$4)", 4, types, args, nulls, false, 0);
	else if (PG_ARGISNULL(2))
		manifest_execute("DELETE FROM pg_catalog.pg_dolt_ref WHERE refname=$1 AND reftoken=$2", 2, types, args, nulls, false, 0);
	else
		manifest_execute("UPDATE pg_catalog.pg_dolt_ref SET reftarget=$3,reftoken=$4 WHERE refname=$1 AND reftoken=$2", 4, types, args, nulls, false, 0);
	if (SPI_processed != 1)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("manifest ref changed; reread and retry transaction")));
	/* Branch and tag moves feed dolt_reflog. */
	if (!PG_ARGISNULL(2) && (strncmp(text_to_cstring(name), "heads/", 6) == 0 || strncmp(text_to_cstring(name), "tags/", 5) == 0))
		manifest_execute("INSERT INTO pg_catalog.pg_dolt_reflog(reflogname,reflogtime,reflogtarget) VALUES($1,clock_timestamp(),$3)", 4, types, args, nulls, false, 0);
	/* Named revision bindings must be reanalyzed after ref publication. */
	CacheInvalidateRelcacheByRelid(DoltRefRelationId);
	SPI_finish();
	if (PG_ARGISNULL(2))
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(token);
}

static bytea *
export_node(PLStore *store, PLHash hash, int depth)
{
	PLNode	   *node = pl_find(store, hash);
	Datum	   *children;
	uint32		i,
				count;

	check_stack_depth();
	if (!node || depth >= PL_MAX_DEPTH)
		elog(ERROR, "invalid prolly export tree");
	count = node->level ? node->count : 0;
	children = palloc(sizeof(Datum) * Max(count, 1));
	for (i = 0; i < count; i++)
	{
		PLHash		child;

		memcpy(child.bytes, pl_node_value(node, i).data, PL_HASH_SIZE);
		children[i] = PointerGetDatum(export_node(store, child, depth + 1));
	}
	return put_object("chunk", bytes(node->data, node->size), children_array(children, count));
}

bytea *
dg_history_export_tree(const PLTree * tree)
{
	bytea	   *result;

	manifest_begin();
	result = export_node(tree->store, tree->root, 0);
	SPI_finish();
	return result;
}

Datum
dolt_manifest_export(PG_FUNCTION_ARGS)
{
	Relation	rel;
	DGState    *state;
	bytea	   *result;

	manifest_begin();
	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	if (rel->rd_tableam != dg_table_routine() &&
		!(rel->rd_rel->relkind == RELKIND_INDEX && rel->rd_rel->relam == get_index_am_oid("dolt_prolly_index", false)))
		dg_unsupported("manifest export of non-prolly relations");
	state = dg_load(rel, GetActiveSnapshot());
	result = export_node(state->store, state->content.root, 0);
	dg_release(state);
	relation_close(rel, AccessShareLock);
	SPI_finish();
	PG_RETURN_BYTEA_P(result);
}

/* Import retained canonical bytes, independently of the source relation's lifetime. */
static PLHash
import_node(PLStore *store, bytea *address, int depth)
{
	Oid			types[] = {BYTEAOID};
	Datum		args[] = {PointerGetDatum(address)};
	bool		isnull;
	bytea	   *payload;
	ArrayType  *children;
	Datum	   *values;
	bool	   *nulls;
	int			n,
				i;
	PLSlice		bytes_;
	PLHash		hash;

	check_stack_depth();
	if (depth >= PL_MAX_DEPTH)
		elog(ERROR, "history chunk tree too deep");
	manifest_execute("SELECT objpayload,objchildren FROM pg_catalog.pg_dolt_object_visible WHERE objhash=$1 AND objkind='chunk'", 1, types, args, NULL, true, 1);
	if (SPI_processed != 1)
		elog(ERROR, "missing history chunk");
	payload = DatumGetByteaPCopy(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	children = DatumGetArrayTypePCopy(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &n);
	for (i = 0; i < n; i++)
		import_node(store, DatumGetByteaP(values[i]), depth + 1);
	bytes_.data = (uint8 *) VARDATA(payload);
	bytes_.len = VARSIZE(payload) - VARHDRSZ;
	if (!pl_hash_bytes(bytes_, &hash))
		elog(ERROR, "history chunk hash failed");
	dg_check(pl_store_import(store, hash, bytes_));
	pfree(payload);
	pfree(children);
	pfree(values);
	pfree(nulls);
	return hash;
}

PLTree
dg_history_import(PLStore *store, bytea *address)
{
	PLTree		result;
	PLHash		hash;

	manifest_begin();
	hash = import_node(store, address, 0);
	dg_check(pl_open(store, hash, &result));
	SPI_finish();
	return result;
}

/* Canonical schema properties use the same node encoding and retained chunk
 * ownership as table contents. Runtime OIDs/transaction metadata never enter
 * this tree. The outer object kind distinguishes table schemas/namespaces. */
bytea *
dg_schema_store(const char *kind, const char *json)
{
	PLStore    *volatile store = NULL;
	bytea	   *result = NULL;
	PLTree		tree;
	PLEntry    *entries;
	int			n,
				i;
	Oid			types[] = {TEXTOID};
	Datum		args[] = {CStringGetTextDatum(json)};

	manifest_begin();
	PG_TRY();
	{
		manifest_execute("SELECT key,value::text FROM jsonb_each($1::jsonb) ORDER BY key COLLATE \"C\"", 1, types, args, NULL, true, 0);
		n = SPI_processed;
		entries = palloc(sizeof(*entries) * Max(n, 1));
		for (i = 0; i < n; i++)
		{
			char	   *key = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
			char	   *value = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);

			key = pg_server_to_any(key, strlen(key), PG_UTF8);
			value = pg_server_to_any(value, strlen(value), PG_UTF8);
			entries[i].key = (PLSlice)
			{
				(uint8 *) key, strlen(key)
			};
			entries[i].value = (PLSlice)
			{
				(uint8 *) value, strlen(value)
			};
		}
		store = pl_store_create();
		if (!store)
			elog(ERROR, "cannot allocate schema tree");
		dg_check(pl_build(store, entries, n, &tree));
		{
			Datum		root = PointerGetDatum(export_node(store, tree.root, 0));

			result = put_object(kind, bytes(DG_SCHEMA_TREE, strlen(DG_SCHEMA_TREE)), children_array(&root, 1));
		}
	}
	PG_FINALLY();
	{
		if (store)
			pl_store_free(store);
	}
	PG_END_TRY();
	SPI_finish();
	return result;
}

char *
dg_schema_read(bytea *address, const char *kind)
{
	PLStore    *volatile store = NULL;
	char	   *result = NULL;
	Oid			types[] = {BYTEAOID, TEXTOID};
	Datum		args[] = {PointerGetDatum(address), CStringGetTextDatum(kind)};
	bool		isnull;
	bytea	   *payload;
	ArrayType  *children;
	Datum	   *values;
	bool	   *nulls;
	int			n;

	manifest_begin();
	manifest_execute("SELECT objpayload,objchildren FROM pg_catalog.pg_dolt_object_visible WHERE objhash=$1 AND objkind=$2", 2, types, args, NULL, true, 1);
	if (SPI_processed != 1)
		elog(ERROR, "missing retained schema object");
	payload = DatumGetByteaPCopy(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	children = DatumGetArrayTypePCopy(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
	deconstruct_array(children, BYTEAOID, -1, false, TYPALIGN_INT, &values, &nulls, &n);
	if (n == 0)
	{
		/* Existing repositories retain their original JSON schema objects. */
		result = pnstrdup(VARDATA_ANY(payload), VARSIZE_ANY_EXHDR(payload));
	}
	else
	{
		if (n != 1 || nulls[0] || VARSIZE_ANY_EXHDR(payload) != strlen(DG_SCHEMA_TREE) ||
			memcmp(VARDATA_ANY(payload), DG_SCHEMA_TREE, strlen(DG_SCHEMA_TREE)))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid retained schema tree envelope")));
		PG_TRY();
		{
			PLTree		tree;
			DGRows		rows;
			size_t		i;
			StringInfoData json;

			store = pl_store_create();
			if (!store)
				elog(ERROR, "cannot allocate schema reader");
			dg_check(pl_open(store, import_node(store, DatumGetByteaP(values[0]), 0), &tree));
			rows = dg_rows(&tree);
			initStringInfo(&json);
			appendStringInfoChar(&json, '{');
			for (i = 0; i < rows.count; i++)
			{
				char	   *key = pnstrdup((char *) rows.items[i].key.data, rows.items[i].key.len);

				if (i)
					appendStringInfoChar(&json, ',');
				escape_json(&json, key);
				appendStringInfoChar(&json, ':');
				appendBinaryStringInfo(&json, (char *) rows.items[i].value.data, rows.items[i].value.len);
			}
			appendStringInfoChar(&json, '}');
			result = json.data;
		}
		PG_FINALLY();
		{
			if (store)
				pl_store_free(store);
		}
		PG_END_TRY();
	}
	result = pg_any_to_server(result, strlen(result), PG_UTF8);
	SPI_finish();
	return result;
}
