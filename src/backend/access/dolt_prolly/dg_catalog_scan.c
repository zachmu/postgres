/* Native read-only scan over a retained catalog binding. Fetch one chunk at a
 * time; no relation creation, heap reconstruction, or whole-tree import. */
#include "dg.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "optimizer/restrictinfo.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "access/doltcatalog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/catalogview.h"
#include "catalog/pg_dolt_object.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#define CAT_ARGS(...) ((Datum[]){__VA_ARGS__})
typedef struct DGFrame
{
	PLStore    *store;
	PLNode	   *node;
	Datum	   *children;
	int			nchildren;
	uint32		index;
	MemoryContext context;
} DGFrame;
typedef struct DGCatalogScan
{
	CustomScanState css;
	DGFrame		frames[PL_MAX_DEPTH];
	int			depth;
	uint64		repeats;
	uint64		chunks;
	bytea	   *root;
	bytea	   *initial_root;
	Relation	objects;
	Snapshot	snapshot;
	DGState    *live;
	DGStorage  *storage;
	CatalogViewScan *catalog_scan;
	PLCursor	cursor;
	MemoryContextCallback cleanup;
} DGCatalogScan;
static void explain_scan(CustomScanState *css, List *ancestors, ExplainState *es);
static void begin_scan(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *exec_scan(CustomScanState *css);
static void end_scan(CustomScanState *css);
static void rescan(CustomScanState *css);
static Node *create_state(CustomScan *plan);
static Plan *create_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *path, List *tlist, List *clauses, List *children);
static const CustomPathMethods path_methods = {.CustomName = "DoltVersionedScan",.PlanCustomPath = create_plan};
static const CustomScanMethods scan_methods = {.CustomName = "DoltVersionedScan",.CreateCustomScanState = create_state};
static const CustomExecMethods exec_methods = {.CustomName = "DoltVersionedScan",.BeginCustomScan = begin_scan,.ExecCustomScan = exec_scan,.EndCustomScan = end_scan,.ReScanCustomScan = rescan,.ExplainCustomScan = explain_scan};
const CustomScanMethods *
dg_catalog_scan_methods(void)
{
	return &scan_methods;
}
void
dg_catalog_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	CustomPath *path = makeNode(CustomPath);

	/*
	 * Until retained statistics/index paths exist, use a conservative
	 * estimate.
	 */
	rel->tuples = 1000;
	set_baserel_size_estimates(root, rel);
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->path.rows = rel->rows;
	path->path.startup_cost = 1;
	path->path.total_cost = 1 + 1000 * cpu_tuple_cost + rel->baserestrictcost.per_tuple * 1000;
	path->flags = 0;
	path->methods = &path_methods;
	add_path(rel, &path->path);
}
static Plan *
create_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *path, List *tlist, List *clauses, List *children)
{
	CustomScan *scan = makeNode(CustomScan);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	scan->scan.plan.targetlist = tlist;
	scan->scan.plan.qual = extract_actual_clauses(clauses, false);
	scan->scan.scanrelid = rel->relid;
	scan->custom_scan_tlist = build_physical_tlist(root, rel);
	scan->custom_relids = bms_copy(rel->relids);
	scan->custom_private = list_make3(makeString(pstrdup(rte->dolt_table)), makeString(pstrdup(rte->dolt_branch ? rte->dolt_branch : "")), makeInteger(rte->dolt_relid));
	scan->custom_private = lappend(scan->custom_private, makeString(pstrdup(rte->dolt_database)));
	scan->custom_private = lappend(scan->custom_private, makeInteger(rte->dolt_catalog));
	scan->custom_private = lappend(scan->custom_private, makeInteger(rte->dolt_catalog ? InvalidOid : get_typ_typrelid(rte->dolt_rowtype)));
	scan->methods = &scan_methods;
	return &scan->scan.plan;
}
static Node *
create_state(CustomScan *plan)
{
	DGCatalogScan *s = palloc0(sizeof(*s));

	NodeSetTag(&s->css, T_CustomScanState);
	s->css.methods = &exec_methods;
	s->css.slotOps = &TTSOpsHeapTuple;
	return (Node *) s;
}
static bytea *
address(const char *hex)
{
	bytea	   *out = palloc(VARHDRSZ + PL_HASH_SIZE);
	int			i;

	if (strlen(hex) != 2 * PL_HASH_SIZE)
		elog(ERROR, "invalid retained address");
	SET_VARSIZE(out, VARHDRSZ + PL_HASH_SIZE);
	for (i = 0; i < PL_HASH_SIZE; i++)
	{
		unsigned	v;

		if (sscanf(hex + 2 * i, "%2x", &v) != 1)
			elog(ERROR, "invalid retained address");
		((uint8 *) VARDATA(out))[i] = v;
	}
	return out;
}
static bytea *
object(DGCatalogScan *s, bytea *hash, const char *kind, Datum **children, int *count)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		isnull;
	bytea	   *data;
	ArrayType  *array;
	bool	   *nulls;
	char	   *actual;

	ScanKeyInit(&key, Anum_pg_dolt_object_objhash, BTEqualStrategyNumber, F_BYTEAEQ, PointerGetDatum(hash));
	scan = systable_beginscan(s->objects, DoltObjectKeyIndexId, true, s->snapshot, 1, &key);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "missing retained catalog object");
	actual = TextDatumGetCString(heap_getattr(tuple, Anum_pg_dolt_object_objkind, RelationGetDescr(s->objects), &isnull));
	if (strcmp(actual, kind))
		elog(ERROR, "unexpected retained object kind");
	data = DatumGetByteaPCopy(heap_getattr(tuple, Anum_pg_dolt_object_objpayload, RelationGetDescr(s->objects), &isnull));
	array = DatumGetArrayTypePCopy(heap_getattr(tuple, Anum_pg_dolt_object_objchildren, RelationGetDescr(s->objects), &isnull));
	deconstruct_array(array, BYTEAOID, -1, false, TYPALIGN_INT, children, &nulls, count);
	systable_endscan(scan);
	pfree(actual);
	pfree(nulls);
	return data;
}
static void
cleanup(void *arg)
{
	DGCatalogScan *s = arg;
	int			i;

	for (i = 0; i < PL_MAX_DEPTH; i++)
		if (s->frames[i].store)
		{
			pl_store_free(s->frames[i].store);
			s->frames[i].store = NULL;
		}
}
static void
push(DGCatalogScan *s, bytea *hash)
{
	DGFrame    *f;
	MemoryContext old;
	bytea	   *payload;
	PLHash		nodehash;
	PLSlice		bytes;

	if (s->depth >= PL_MAX_DEPTH)
		elog(ERROR, "retained tree too deep");
	s->chunks++;
	f = &s->frames[s->depth++];
	f->context = AllocSetContextCreate(s->css.ss.ps.state->es_query_cxt, "retained chunk", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(f->context);
	payload = object(s, hash, "chunk", &f->children, &f->nchildren);
	bytes.data = (uint8 *) VARDATA_ANY(payload);
	bytes.len = VARSIZE_ANY_EXHDR(payload);
	f->store = pl_store_create();
	if (!f->store)
		elog(ERROR, "cannot allocate retained chunk reader");
	if (!pl_hash_bytes(bytes, &nodehash))
		elog(ERROR, "retained chunk hash failure");
	dg_check(pl_store_import(f->store, nodehash, bytes));
	f->node = pl_find(f->store, nodehash);
	f->index = 0;
	if ((f->node->level && f->nchildren != f->node->count) || (!f->node->level && f->nchildren))
		elog(ERROR, "invalid retained chunk children");
	MemoryContextSwitchTo(old);
}
static void
pop(DGCatalogScan *s)
{
	DGFrame    *f = &s->frames[--s->depth];

	pl_store_free(f->store);
	f->store = NULL;
	MemoryContextDelete(f->context);
	f->context = NULL;
}
static void
begin_scan(CustomScanState *css, EState *estate, int eflags)
{
	DGCatalogScan *s = (DGCatalogScan *) css;
	CustomScan *plan = (CustomScan *) css->ss.ps.plan;
	Datum	   *children;
	int			count;
	char	   *table = strVal(linitial(plan->custom_private));

	if (intVal(list_nth(plan->custom_private, 4)))
	{
		const char *branch = strVal(lsecond(plan->custom_private));
		CatalogView *view = dg_catalog_view(strVal(list_nth(plan->custom_private, 3)), *branch ? branch : NULL);
		CatalogView *previous;

		s->objects = table_open((Oid) intVal(list_nth(plan->custom_private, 4)), AccessShareLock);
		previous = CatalogViewSwitch(view);
		PG_TRY();
		{
			s->catalog_scan = CatalogViewBeginScan(s->objects, 0, NULL);
		}
		PG_FINALLY();
		{
			CatalogViewSwitch(previous);
		}
		PG_END_TRY();
		if (!s->catalog_scan)
			elog(ERROR, "canonical catalog scan has no provider");
		return;
	}

	dg_catalog_permission(table, GetUserId());
	s->snapshot = estate->es_snapshot;
	s->objects = table_open(DoltObjectRelationId, AccessShareLock);
	object(s, address(table), "table", &children, &count);
	if (count != 2)
		elog(ERROR, "invalid retained table descriptor");
	s->root = DatumGetByteaPCopy(children[1]);
	s->initial_root = s->root;
	s->cleanup.func = cleanup;
	s->cleanup.arg = s;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, &s->cleanup);

	/*
	 * Load lazily on first request, including LIMIT 0 and EXPLAIN without
	 * ANALYZE.
	 */
	s->depth = 0;
	s->repeats = 0;
	if (intVal(lthird(plan->custom_private)))
	{
		Oid			definition = (Oid) intVal(list_nth(plan->custom_private, 5));
		char	   *name = dg_qualified_name(get_namespace_name(get_rel_namespace(definition)), get_rel_name(definition));

		s->storage = dg_storage_open(definition, strVal(lsecond(plan->custom_private)), name,
									 DatumGetByteaP(children[0]), (Oid) intVal(lthird(plan->custom_private)), s->snapshot);
		s->live = dg_storage_load(s->storage);
		if (s->live->has_root)
		{
			dg_check(pl_cursor_init(&s->cursor, &s->live->content, NULL));
			s->root = NULL;
		}
		else
		{
			dg_release(s->live);
			s->live = NULL;
		}
	}
}
static TupleTableSlot *
next_row(ScanState *ss)
{
	DGCatalogScan *s = (DGCatalogScan *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	ExecClearTuple(slot);
	if (s->catalog_scan)
	{
		HeapTuple	tuple = CatalogViewNext(s->catalog_scan, ForwardScanDirection);

		return tuple ? ExecStoreHeapTuple(tuple, slot, false) : slot;
	}

	if (s->live)
	{
		PLEntry		entry;
		PLSlice		value;
		HeapTuple	tuple;

		if (!s->cursor.valid)
			return slot;
		entry = pl_cursor_entry(&s->cursor);
		value = entry.value;
		if (value.len < 8)
			elog(ERROR, "invalid working row");
		if (!s->repeats)
			s->repeats = pl_read64(value.data);
		if (!s->repeats)
			elog(ERROR, "invalid working cardinality");
		value.data += 8;
		value.len -= 8;
		tuple = dg_decode_descriptor(slot->tts_tupleDescriptor, value);
		if (--s->repeats == 0)
			dg_check(pl_cursor_next(&s->cursor));
		return ExecStoreHeapTuple(tuple, slot, true);
	}
	if (s->depth == 0 && s->root)
	{
		push(s, s->root);
		s->root = NULL;
	}
	while (s->depth)
	{
		DGFrame    *f = &s->frames[s->depth - 1];
		PLSlice		value;
		HeapTuple	tuple;

		CHECK_FOR_INTERRUPTS();
		if (f->index == f->node->count)
		{
			pop(s);
			continue;
		}
		if (f->node->level)
		{
			bytea	   *child = DatumGetByteaP(f->children[f->index++]);

			push(s, child);
			continue;
		}
		value = pl_node_value(f->node, f->index);
		if (value.len < 8)
			elog(ERROR, "invalid retained row");
		if (!s->repeats)
			s->repeats = pl_read64(value.data);
		if (!s->repeats)
			elog(ERROR, "invalid retained row cardinality");
		value.data += 8;
		value.len -= 8;
		tuple = dg_decode_descriptor(slot->tts_tupleDescriptor, value);
		if (--s->repeats == 0)
			f->index++;
		return ExecStoreHeapTuple(tuple, slot, true);
	}
	return slot;
}
static bool
recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}
static TupleTableSlot *
exec_scan(CustomScanState *css)
{
	return ExecScan(&css->ss, next_row, recheck);
}
static void
end_scan(CustomScanState *css)
{
	DGCatalogScan *s = (DGCatalogScan *) css;

	if (s->catalog_scan)
	{
		CatalogViewEndScan(s->catalog_scan);
		s->catalog_scan = NULL;
	}

	while (s->depth)
		pop(s);
	if (s->live)
	{
		dg_release(s->live);
		s->live = NULL;
	}
	if (s->storage)
	{
		dg_storage_close(s->storage);
		s->storage = NULL;
	}
	if (s->objects)
	{
		table_close(s->objects, AccessShareLock);
		s->objects = NULL;
	}
}
static void
rescan(CustomScanState *css)
{
	DGCatalogScan *s = (DGCatalogScan *) css;

	if (s->catalog_scan)
	{
		CatalogViewRescan(s->catalog_scan, NULL);
		ExecScanReScan(&css->ss);
		return;
	}

	while (s->depth)
		pop(s);
	s->repeats = 0;
	ExecScanReScan(&css->ss);
	if (s->live)
	{
		dg_check(pl_cursor_init(&s->cursor, &s->live->content, NULL));
		return;
	}
	s->root = s->initial_root;
}

static void
explain_scan(CustomScanState *css, List *ancestors, ExplainState *es)
{
	if (es->analyze)
		ExplainPropertyInteger("Chunks read", NULL, ((DGCatalogScan *) css)->chunks, es);
}
