#include "dg.h"
#include "access/amapi.h"
#include "access/nbtree.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_namespace_d.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"

typedef struct DGIndexEntry
{
	PLEntry		entry;
	size_t		prefix;
	bool		nullkey;
}			DGIndexEntry;
typedef struct DGIndexScan
{
	MemoryContext context;
	ItemPointerData *tids;
	HeapTuple  *tuples;
	size_t		count;
	int64		position,
				mark;
}			DGIndexScan;
static int
compare_entries(const void *a, const void *b)
{
	return pl_compare(((const DGIndexEntry *) a)->entry.key, ((const DGIndexEntry *) b)->entry.key);
}
static void
escaped(StringInfo out, PLSlice bytes, bool descending)
{
	size_t		i;
	uint8		xor = descending ? 255 : 0;

	for (i = 0; i < bytes.len; i++)
	{
		appendStringInfoCharMacro(out, bytes.data[i] ^ xor);
		if (!bytes.data[i])
			appendStringInfoCharMacro(out, 255 ^ xor);
	}
	appendStringInfoCharMacro(out, 0 ^ xor);
	appendStringInfoCharMacro(out, 0 ^ xor);
}
void
dg_index_rebuild(Relation table, Relation index)
{
	DGState    *source,
			   *target;
	DGRows		rows;
	DGIndexEntry *sorted;
	PLEntry    *entries;
	size_t		i;
	int			j;

	if (table->rd_tableam != dg_table_routine())
		dg_unsupported("prolly indexes on another table AM");
	if (RelationGetIndexExpressions(index) != NIL || RelationGetIndexPredicate(index) != NIL)
		dg_unsupported("expression and partial indexes");
	if (index->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
		dg_unsupported("temporary or unlogged indexes");
	for (j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		int			att = index->rd_index->indkey.values[j];
		Oid			type,
					function = index->rd_support[j];
		const char *expected;
		static const char *operators[] = {"<", "<=", "=", ">=", ">"};
		int			strategy;

		if (att <= 0)
			dg_unsupported("system-column indexes");
		type = TupleDescAttr(RelationGetDescr(table), att - 1)->atttypid;
		if (type != BOOLOID && type != INT2OID && type != INT4OID &&
			type != INT8OID && type != TEXTOID && type != BYTEAOID)
			dg_unsupported("ordered index keys of this type");
		if (type == TEXTOID && !pg_newlocale_from_collation(index->rd_indcollation[j])->collate_is_c)
			dg_unsupported("non-C index collations");
		expected = type == BOOLOID ? "btboolcmp" : type == INT2OID ? "btint2cmp" : type == INT4OID ? "btint4cmp" : type == INT8OID ? "btint8cmp" : type == TEXTOID ? "bttextcmp" : "byteacmp";
		if (get_func_namespace(function) != PG_CATALOG_NAMESPACE || strcmp(get_func_name(function), expected))
			dg_unsupported("custom index comparators");
		for (strategy = 1; strategy <= 5; strategy++)
		{
			Oid			actual = get_opfamily_member(index->rd_opfamily[j], type, type, strategy);
			Oid			wanted = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString(pstrdup(operators[strategy - 1]))), type, type);

			if (!OidIsValid(actual) || actual != wanted)
				dg_unsupported("custom index operators");
		}

	}
	dg_validate_descriptor(RelationGetDescr(table));
	source = dg_load(table, SnapshotSelf);
	target = dg_load(index, SnapshotSelf);
	rows = dg_rows(&source->content);
	sorted = palloc0(Max(rows.count, 1) * sizeof(*sorted));
	entries = palloc0(Max(rows.count, 1) * sizeof(*entries));
	for (i = 0; i < rows.count; i++)
	{
		StringInfoData key;
		uint64		count = pl_read64(rows.items[i].value.data);
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		uint8	   *value;

		initStringInfo(&key);
		for (j = 0; j < index->rd_index->indnkeyatts; j++)
		{
			int			attribute = index->rd_index->indkey.values[j] - 1;
			PLValue		field;
			bool		desc = (index->rd_indoption[j] & INDOPTION_DESC) != 0;
			bool		nullfirst = (index->rd_indoption[j] & INDOPTION_NULLS_FIRST) != 0;

			if (attribute < 0)
				dg_unsupported("system-column indexes");
			dg_check(pl_tuple_get(row, attribute, &field));
			appendStringInfoChar(&key, field.present ? 1 : (nullfirst ? 0 : 2));
			if (field.present)
				escaped(&key, field.bytes, desc);
			else
				sorted[i].nullkey = true;
		}
		sorted[i].prefix = key.len;
		escaped(&key, rows.items[i].key, false);
		sorted[i].entry.key.data = (uint8 *) key.data;
		sorted[i].entry.key.len = key.len;
		value = palloc(8 + row.len);
		pl_write64(value, count);
		memcpy(value + 8, row.data, row.len);
		sorted[i].entry.value.data = value;
		sorted[i].entry.value.len = 8 + row.len;
	}
	qsort(sorted, rows.count, sizeof(*sorted), compare_entries);
	for (i = 0; i < rows.count; i++)
	{
		if (index->rd_index->indisunique && (!sorted[i].nullkey || index->rd_index->indnullsnotdistinct))
		{
			bool		duplicate = pl_read64(sorted[i].entry.value.data) > 1;

			if (i && sorted[i].prefix == sorted[i - 1].prefix && memcmp(sorted[i].entry.key.data, sorted[i - 1].entry.key.data, sorted[i].prefix) == 0)
				duplicate = true;
			if (duplicate)
				ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("duplicate key violates unique constraint \"%s\"", RelationGetRelationName(index))));
		}
		entries[i] = sorted[i].entry;
	}
	dg_check(pl_build(target->store, entries, rows.count, &target->content));
	dg_save(index, target, GetCurrentCommandId(true));
	dg_release(source);
	dg_release(target);
}
void
dg_rebuild_indexes(Relation rel)
{
	List	   *indexes;
	ListCell   *cell;

	if (rel->rd_catalogview)
	{
		foreach(cell, dg_storage_indexes(rel))
			dg_index_rebuild(rel, (Relation) lfirst(cell));
		return;
	}
	indexes = RelationGetIndexList(rel);

	foreach(cell, indexes)
	{
		Relation	index = index_open(lfirst_oid(cell), RowExclusiveLock);

		if (strcmp(get_am_name(index->rd_rel->relam), "dolt_prolly_index") != 0)
			dg_unsupported("other index families on prolly tables");
		if (index->rd_index->indisready)
			dg_index_rebuild(rel, index);
		index_close(index, RowExclusiveLock);
	}
	list_free(indexes);
}
static IndexBuildResult *
build(Relation table, Relation index, IndexInfo *info)
{
	IndexBuildResult *result = palloc0(sizeof(*result));
	DGState    *s;

	if (info->ii_Concurrent)
		dg_unsupported("concurrent index builds");
	dg_index_rebuild(table, index);
	s = dg_load(table, SnapshotSelf);
	result->heap_tuples = result->index_tuples = pl_count(&s->handles);
	dg_release(s);
	return result;
}
static void
build_empty(Relation rel)
{
	dg_unsupported("unlogged indexes");
}
static bool
insert(Relation index, Datum *values, bool *nulls, ItemPointer tid, Relation table, IndexUniqueCheck unique, bool unchanged, IndexInfo *info)
{
	if (unique == UNIQUE_CHECK_PARTIAL || unique == UNIQUE_CHECK_EXISTING)
		dg_unsupported("deferred unique constraints");
	dg_index_rebuild(table, index);
	return true;
}
static IndexBulkDeleteResult *
bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback cb, void *arg)
{
	dg_unsupported("index vacuum");
}
static IndexBulkDeleteResult *
vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats ? stats : palloc0(sizeof(*stats));
}
static void
cost(PlannerInfo *root, IndexPath *path, double loops, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loops, &costs);
	*startup = costs.indexStartupCost;
	*total = costs.indexTotalCost;
	*selectivity = costs.indexSelectivity;
	*correlation = 0;
	*pages = costs.numIndexPages;
}
static bytea *
options(Datum reloptions, bool validate)
{
	if (DatumGetPointer(reloptions))
		dg_unsupported("index storage options");
	return NULL;
}
static bool
validate(Oid opclass)
{
	return true;
}
static IndexScanDesc
beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);
	DGIndexScan *s = palloc0(sizeof(*s));

	if (norderbys)
		dg_unsupported("ORDER BY operators");
	s->context = AllocSetContextCreate(CurrentMemoryContext, "prolly index scan", ALLOCSET_DEFAULT_SIZES);
	s->position = -1;
	scan->opaque = s;
	return scan;
}
static bool
matches(IndexScanDesc scan, TupleTableSlot *slot)
{
	int			i;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		int			att = scan->indexRelation->rd_index->indkey.values[key->sk_attno - 1];
		bool		null;
		Datum		value = slot_getattr(slot, att, &null);

		if (key->sk_flags & SK_SEARCHNULL)
		{
			if (!null)
				return false;
		}
		else if (key->sk_flags & SK_SEARCHNOTNULL)
		{
			if (null)
				return false;
		}
		else
		{
			if (key->sk_flags & ~SK_ISNULL)
				dg_unsupported("this index scan-key flag");
			if (null || key->sk_flags & SK_ISNULL || !DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, value, key->sk_argument)))
				return false;
		}
	}
	return true;
}
static void
rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	DGIndexScan *s = scan->opaque;
	MemoryContext old;
	DGState    *index,
			   *table;
	DGRows		rows,
				handles;
	size_t		i,
				j;
	TupleTableSlot *slot;

	if (keys && nkeys)
		memcpy(scan->keyData, keys, sizeof(ScanKeyData) * nkeys);
	MemoryContextReset(s->context);
	old = MemoryContextSwitchTo(s->context);
	index = dg_load(scan->indexRelation, scan->xs_snapshot);
	table = dg_load(scan->heapRelation, scan->xs_snapshot);
	rows = dg_rows(&index->content);
	handles = dg_rows(&table->handles);
	s->tids = palloc(Max(handles.count, 1) * sizeof(ItemPointerData));
	s->tuples = palloc0(Max(handles.count, 1) * sizeof(HeapTuple));
	s->count = 0;
	s->position = -1;
	s->mark = -1;
	slot = table_slot_create(scan->heapRelation, NULL);
	for (i = 0; i < rows.count; i++)
	{
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		PLHash		hash = dg_rowhash(row);
		PLSlice		rowkey = {hash.bytes, PL_HASH_SIZE};

		for (j = 0; j < handles.count; j++)
			if (pl_compare(rowkey, handles.items[j].value) == 0)
			{
				ItemPointerData tid;

				dg_tid(pl_read64(handles.items[j].key.data), &tid);
				dg_decode(scan->heapRelation, row, &tid, slot);
				if (matches(scan, slot))
				{
					if (scan->xs_want_itup)
					{
						Datum		values[INDEX_MAX_KEYS];
						bool		nulls[INDEX_MAX_KEYS];
						int			k;

						for (k = 0; k < scan->indexRelation->rd_index->indnatts; k++)
							values[k] = slot_getattr(slot, scan->indexRelation->rd_index->indkey.values[k], &nulls[k]);
						s->tuples[s->count] = heap_form_tuple(RelationGetDescr(scan->indexRelation), values, nulls);
					}
					s->tids[s->count++] = tid;
				}
			}
	}
	ExecDropSingleTupleTableSlot(slot);
	dg_release(index);
	dg_release(table);
	MemoryContextSwitchTo(old);
}
static bool
gettuple(IndexScanDesc scan, ScanDirection dir)
{
	DGIndexScan *s = scan->opaque;

	if (ScanDirectionIsBackward(dir))
	{
		if (s->position < 0)
			s->position = s->count;
		--s->position;
	}
	else
		++s->position;
	if (s->position < 0 || s->position >= (int64) s->count)
		return false;
	scan->xs_heaptid = s->tids[s->position];
	scan->xs_recheck = false;
	scan->xs_hitup = s->tuples[s->position];
	scan->xs_hitupdesc = RelationGetDescr(scan->indexRelation);
	return true;
}
static void
endscan(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	MemoryContextDelete(s->context);
	pfree(s);
}
static void
markpos(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	s->mark = s->position;
}
static void
restorepos(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	s->position = s->mark;
}
static bool
canreturn(Relation index, int attribute)
{
	return true;
}
Datum
dolt_prolly_index_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 5;
	am->amsupport = 1;
	am->amcanorder = true;
	am->amcanbackward = true;
	am->amcanunique = true;
	am->amcanmulticol = true;
	am->amoptionalkey = true;
	am->amsearchnulls = true;
	am->amcaninclude = true;
	am->amconsistentequality = true;
	am->amconsistentordering = true;
	am->ambuild = build;
	am->ambuildempty = build_empty;
	am->aminsert = insert;
	am->ambulkdelete = bulkdelete;
	am->amvacuumcleanup = vacuumcleanup;
	am->amcostestimate = cost;
	am->amcanreturn = canreturn;
	am->amoptions = options;
	am->amvalidate = validate;
	am->ambeginscan = beginscan;
	am->amrescan = rescan;
	am->amgettuple = gettuple;
	am->amendscan = endscan;
	am->ammarkpos = markpos;
	am->amrestrpos = restorepos;
	am->amtranslatestrategy = bttranslatestrategy;
	am->amtranslatecmptype = bttranslatecmptype;
	PG_RETURN_POINTER(am);
}
