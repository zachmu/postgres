/*
 * Read-only scan of a system catalog as a branch or revision sees it: the
 * catalog rows of that ref's canonical schema, not the physical catalog.
 * Tables themselves are ordinary relations; only catalogs use this scan.
 */
#include "dg.h"
#include "parser/parsetree.h"
#include "optimizer/restrictinfo.h"
#include "access/doltcatalog.h"
#include "access/table.h"
#include "catalog/catalogview.h"
#include "executor/executor.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "utils/rel.h"

typedef struct DGCatalogScan
{
	CustomScanState css;
	Relation	catalog;
	CatalogViewScan *scan;
} DGCatalogScan;
static void begin_scan(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *exec_scan(CustomScanState *css);
static void end_scan(CustomScanState *css);
static void rescan(CustomScanState *css);
static Node *create_state(CustomScan *plan);
static Plan *create_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *path, List *tlist, List *clauses, List *children);
static const CustomPathMethods path_methods = {.CustomName = "DoltCatalogScan",.PlanCustomPath = create_plan};
static const CustomScanMethods scan_methods = {.CustomName = "DoltCatalogScan",.CreateCustomScanState = create_state};
static const CustomExecMethods exec_methods = {.CustomName = "DoltCatalogScan",.BeginCustomScan = begin_scan,.ExecCustomScan = exec_scan,.EndCustomScan = end_scan,.ReScanCustomScan = rescan};

const CustomScanMethods *
dg_catalog_scan_methods(void)
{
	return &scan_methods;
}

void
dg_catalog_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	CustomPath *path = makeNode(CustomPath);

	/* Catalog views have no statistics; use a conservative estimate. */
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

/* custom_private: the ref's branch ("" for a revision), schema hash, catalog. */
static Plan *
create_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *path, List *tlist, List *clauses, List *children)
{
	CustomScan *scan = makeNode(CustomScan);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	Assert(OidIsValid(rte->dolt_catalog));
	scan->scan.plan.targetlist = tlist;
	scan->scan.plan.qual = extract_actual_clauses(clauses, false);
	scan->scan.scanrelid = rel->relid;
	scan->custom_scan_tlist = build_physical_tlist(root, rel);
	scan->custom_relids = bms_copy(rel->relids);
	scan->custom_private = list_make3(makeString(pstrdup(rte->dolt_branch ? rte->dolt_branch : "")),
									  makeString(pstrdup(rte->dolt_database)),
									  makeInteger(rte->dolt_catalog));
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

static void
begin_scan(CustomScanState *css, EState *estate, int eflags)
{
	DGCatalogScan *s = (DGCatalogScan *) css;
	CustomScan *plan = (CustomScan *) css->ss.ps.plan;
	const char *branch = strVal(linitial(plan->custom_private));
	CatalogView *view = dg_catalog_view(strVal(lsecond(plan->custom_private)), *branch ? branch : NULL);
	CatalogView *previous;

	s->catalog = table_open((Oid) intVal(lthird(plan->custom_private)), AccessShareLock);
	previous = CatalogViewSwitch(view);
	PG_TRY();
	{
		s->scan = CatalogViewBeginScan(s->catalog, 0, NULL);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	if (!s->scan)
		elog(ERROR, "canonical catalog scan has no provider");
}

static TupleTableSlot *
next_row(ScanState *ss)
{
	DGCatalogScan *s = (DGCatalogScan *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	HeapTuple	tuple;

	ExecClearTuple(slot);
	tuple = CatalogViewNext(s->scan, ForwardScanDirection);
	return tuple ? ExecStoreHeapTuple(tuple, slot, false) : slot;
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

	if (s->scan)
	{
		CatalogViewEndScan(s->scan);
		s->scan = NULL;
	}
	if (s->catalog)
	{
		table_close(s->catalog, AccessShareLock);
		s->catalog = NULL;
	}
}

static void
rescan(CustomScanState *css)
{
	DGCatalogScan *s = (DGCatalogScan *) css;

	CatalogViewRescan(s->scan, NULL);
	ExecScanReScan(&css->ss);
}
