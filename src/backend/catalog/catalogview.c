/*
 * Canonical providers publish immutable catalog tuples through this boundary.
 * A view owns its rows and isolated native catcaches. Physical catalog rows are
 * not consulted when the view provides a catalog, including an empty catalog.
 */
#include "postgres.h"
#include "access/xact.h"
#include "access/transam.h"
#include "access/parallel.h"
#include "utils/syscache.h"
#include "catalog/pg_namespace.h"
#include "fmgr.h"
#include "utils/lsyscache.h"
#include "access/htup_details.h"
#include "catalog/catalogview.h"
#include "executor/tuptable.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/hsearch.h"
#include "catalog/pg_type_d.h"
#include "utils/snapshot.h"
#include "utils/sortsupport.h"
#include "catalog/pg_index.h"

typedef struct CatalogViewRows
{
	Oid			catalog;
	List	   *tuples;
} CatalogViewRows;

struct CatalogView
{
	MemoryContext memory;
	MemoryContext rows_memory;
	List	   *catalog_ids;
	uint64		touched;
	TransactionId creator;
	unsigned	scan_references;
	bool		loaded;
	bool		loading;
	void		(*loader) (CatalogView *, void *);
	void	   *loader_arg;
	char	   *identity;
	uint64		serial;
	uint64		references;
	bool		sealed;
	List	   *catalogs;
	List	   *caches;
	List	   *handles;
	dlist_node	link;
};
typedef struct CatalogViewPin
{
	MemoryContextCallback callback;
	CatalogView *view;
} CatalogViewPin;

struct CatalogViewScan
{
	CatalogView *view;
	CatalogViewPin *pin;
	Relation	relation;
	List	   *rows;
	HeapTuple  *ordered;
	int			position;
	bool		started;
	int			nkeys;
	ScanKey		keys;
};
typedef struct CatalogViewObject
{
	Oid			handle;
	Oid			objectclass;
	Oid			storage;
	CatalogView *view;
	char	   *identity;
} CatalogViewObject;
static HTAB *objects;
static Oid	next_handle = FirstCatalogViewObjectId;

static uint64 next_view_serial = 1;
static uint64 view_clock;
int			catalog_view_cache_entries = 16;
static void prune_catalog_views(XactEvent event, void *arg);
static bool prune_registered;
static CatalogView *current_view;
static dlist_head views = DLIST_STATIC_INIT(views);

CatalogView *
CatalogViewCreate(const char *identity)
{
	MemoryContext memory;
	MemoryContext previous;
	CatalogView *view;

	if (IsParallelWorker())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("revision catalog contexts cannot be constructed in parallel workers")));
	memory = AllocSetContextCreate(TopMemoryContext, "catalog view", ALLOCSET_DEFAULT_SIZES);
	previous = MemoryContextSwitchTo(memory);
	view = palloc0(sizeof(*view));

	view->memory = memory;
	view->rows_memory = AllocSetContextCreate(memory, "catalog view rows", ALLOCSET_DEFAULT_SIZES);
	view->touched = ++view_clock;
	if (!prune_registered)
	{
		RegisterXactCallback(prune_catalog_views, NULL);
		prune_registered = true;
	}
	view->identity = pstrdup(identity);
	if (!next_view_serial)
		elog(ERROR, "catalog context identifier space exhausted");
	view->serial = next_view_serial++;
	view->references = 1;
	dlist_push_tail(&views, &view->link);
	MemoryContextSwitchTo(previous);
	return view;
}

void
CatalogViewAddRows(CatalogView *view, Oid catalog, List *tuples)
{
	MemoryContext previous;
	CatalogViewRows *rows;
	ListCell   *lc;
	uint32		ordinal = 0;

	if (view->sealed && !view->loading)
		elog(ERROR, "cannot modify published catalog view");
	foreach(lc, view->catalogs)
	{
		if (((CatalogViewRows *) lfirst(lc))->catalog == catalog)
			elog(ERROR, "duplicate catalog rows in view");
	}
	previous = MemoryContextSwitchTo(view->memory);
	if (!list_member_oid(view->catalog_ids, catalog))
		view->catalog_ids = lappend_oid(view->catalog_ids, catalog);
	MemoryContextSwitchTo(view->rows_memory);
	rows = palloc0(sizeof(*rows));
	rows->catalog = catalog;
	foreach(lc, tuples)
	{
		HeapTuple	tuple = heap_copytuple(lfirst(lc));

		/*
		 * Catcache lists need a tuple identity; it is local to this
		 * catalog/view.
		 */
		ItemPointerSet(&tuple->t_self, ordinal / MaxOffsetNumber + 1, ordinal % MaxOffsetNumber + 1);
		ordinal++;
		rows->tuples = lappend(rows->tuples, tuple);
	}
	view->catalogs = lappend(view->catalogs, rows);
	MemoryContextSwitchTo(previous);
}

void
CatalogViewSeal(CatalogView *view)
{
	view->sealed = true;
	view->loaded = true;
}

void
CatalogViewRetain(CatalogView *view)
{
	Assert(view->references > 0);
	view->references++;
}

void
CatalogViewRelease(CatalogView *view)
{
	ListCell   *lc;

	Assert(view->references > 0);
	if (--view->references)
		return;
	Assert(current_view != view);
	foreach(lc, view->caches)
		DestroyCatCache(lfirst(lc));
	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);

		hash_search(objects, &handle, HASH_REMOVE, NULL);
	}
	dlist_delete(&view->link);
	MemoryContextDelete(view->memory);
}

CatalogView *
CatalogViewCurrent(void)
{
	return current_view;
}

CatalogView *
CatalogViewSwitch(CatalogView *view)
{
	CatalogView *previous = current_view;

	Assert(!view || view->sealed);
	if (view)
		CatalogViewEnsureLoaded(view);
	current_view = view;
	return previous;
}

bool
CatalogViewContains(CatalogView *view, Oid catalog)
{
	return view && list_member_oid(view->catalog_ids, catalog);
}

CatCache *
CatalogViewCache(CatalogView *view, CatCache *base)
{
	ListCell   *lc;
	CatCache   *cache;
	MemoryContext previous;

	if (!CatalogViewContains(view, base->cc_reloid))
		return base;
	CatalogViewEnsureLoaded(view);
	foreach(lc, view->caches)
	{
		cache = lfirst(lc);
		if (cache->id == base->id)
			return cache;
	}
	cache = InitCatCache(base->id, base->cc_reloid, base->cc_indexoid,
						 base->cc_nkeys, base->cc_keyno, base->cc_nbuckets);
	cache->cc_catalogview = view;
	previous = MemoryContextSwitchTo(view->memory);
	view->caches = lappend(view->caches, cache);
	MemoryContextSwitchTo(previous);
	return cache;
}

void
CatalogViewInvalidateCache(int cacheid, uint32 hashvalue)
{
	dlist_iter	it;

	dlist_foreach(it, &views)
	{
		CatalogView *view = dlist_container(CatalogView, link, it.cur);
		ListCell   *lc;

		foreach(lc, view->caches)
		{
			CatCache   *cache = lfirst(lc);

			if (cache->id == cacheid)
				CatCacheInvalidate(cache, hashvalue);
		}
	}
}

static void
release_scan_pin(void *arg)
{
	CatalogViewPin *pin = arg;

	if (pin->view)
	{
		CatalogView *view = pin->view;

		pin->view = NULL;
		Assert(view->scan_references > 0);
		view->scan_references--;
		CatalogViewRelease(view);
	}
}

CatalogViewScan *
CatalogViewBeginScan(Relation relation, int nkeys, ScanKey keys)
{
	CatalogViewScan *scan;
	ListCell   *lc;
	CatalogView *view = current_view;
	bool		has_bound_key = false;
	int			i;


	for (i = 0; i < nkeys; i++)
	{
		int			attr = keys[i].sk_attno;

		if (!(keys[i].sk_flags & (SK_ISNULL | SK_SEARCHARRAY | SK_ROW_HEADER)) && attr > 0 && TupleDescAttr(RelationGetDescr(relation), attr - 1)->atttypid == OIDOID)
		{
			CatalogView *bound = CatalogViewObjectContext(DatumGetObjectId(keys[i].sk_argument));

			if (bound)
			{
				view = bound;
				has_bound_key = true;
				break;
			}
		}
	}
	if (!CatalogViewContains(view, RelationGetRelid(relation)))
	{
		if (has_bound_key)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("catalog scan of %u cannot follow this revision-bound handle", RelationGetRelid(relation))));
		return NULL;
	}
	CatalogViewEnsureLoaded(view);
	scan = palloc0(sizeof(*scan));
	scan->view = view;
	view->scan_references++;
	CatalogViewRetain(scan->view);
	scan->pin = palloc0(sizeof(CatalogViewPin));
	scan->pin->view = scan->view;
	scan->pin->callback.func = release_scan_pin;
	scan->pin->callback.arg = scan->pin;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &scan->pin->callback);
	scan->relation = relation;
	scan->nkeys = nkeys;
	scan->keys = palloc(sizeof(ScanKeyData) * Max(nkeys, 1));
	if (nkeys)
		memcpy(scan->keys, keys, sizeof(ScanKeyData) * nkeys);
	foreach(lc, view->catalogs)
	{
		CatalogViewRows *rows = lfirst(lc);

		if (rows->catalog == RelationGetRelid(relation))
		{
			scan->rows = rows->tuples;
			break;
		}
	}
	return scan;
}

HeapTuple
CatalogViewNext(CatalogViewScan *scan, ScanDirection direction)
{
	while (true)
	{
		HeapTuple	tuple;
		bool		match = true;
		int			i;

		if (!scan->started)
		{
			scan->position = ScanDirectionIsBackward(direction) ? list_length(scan->rows) : -1;
			scan->started = true;
		}
		if (ScanDirectionIsBackward(direction))
		{
			if (scan->position > -1)
				scan->position--;
			if (scan->position < 0)
				return NULL;
		}
		else
		{
			if (scan->position < list_length(scan->rows))
				scan->position++;
			if (scan->position >= list_length(scan->rows))
				return NULL;
		}
		tuple = scan->ordered ? scan->ordered[scan->position] : list_nth(scan->rows, scan->position);
		for (i = 0; i < scan->nkeys; i++)
		{
			ScanKey		key = &scan->keys[i];
			bool		isnull;
			Datum		value = heap_getattr(tuple, key->sk_attno, RelationGetDescr(scan->relation), &isnull);

			if (key->sk_flags & (SK_ROW_HEADER | SK_SEARCHARRAY))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported versioned catalog scan key")));
			if (key->sk_flags & SK_SEARCHNULL)
				match = isnull;
			else if (key->sk_flags & SK_SEARCHNOTNULL)
				match = !isnull;
			else
				match = !isnull && !(key->sk_flags & SK_ISNULL) &&
					DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, value, key->sk_argument));
			if (!match)
				break;
		}
		if (match)
			return tuple;
	}
}

void
CatalogViewRescan(CatalogViewScan *scan, ScanKey keys)
{
	scan->position = 0;
	scan->started = false;
	if (keys && scan->nkeys)
		memcpy(scan->keys, keys, sizeof(ScanKeyData) * scan->nkeys);
}

void
CatalogViewEndScan(CatalogViewScan *scan)
{
	release_scan_pin(scan->pin);
	if (scan->ordered)
		pfree(scan->ordered);
	pfree(scan->keys);
	pfree(scan);
}

Oid
CatalogViewBindObject(CatalogView *view, Oid objectclass, const char *identity, Oid storage)
{
	CatalogViewObject *object;
	MemoryContext previous;
	ListCell   *lc;
	bool		found;

	if (!objects)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(CatalogViewObject);
		ctl.hcxt = TopMemoryContext;
		objects = hash_create("catalog context handles", 128, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);

		object = hash_search(objects, &handle, HASH_FIND, NULL);
		if (object->objectclass == objectclass && !strcmp(object->identity, identity))
		{
			if (object->storage != storage)
				elog(ERROR, "catalog handle storage changed within immutable context");
			return handle;
		}
	}
	if (view->sealed)
		elog(ERROR, "cannot bind a new object in a published catalog context");

	/*
	 * Keep the high-water mark representable so stale handles stay
	 * detectable.
	 */
	if (next_handle == PG_UINT32_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("backend catalog handle space exhausted")));
	object = hash_search(objects, &next_handle, HASH_ENTER, &found);
	Assert(!found);
	object->view = view;
	object->objectclass = objectclass;
	object->storage = storage;
	previous = MemoryContextSwitchTo(view->memory);
	object->identity = pstrdup(identity);
	view->handles = lappend_oid(view->handles, next_handle);
	MemoryContextSwitchTo(previous);
	return next_handle++;
}

static CatalogViewObject *
lookup_object(Oid handle)
{
	CatalogViewObject *object;

	if (handle >= FirstCatalogViewObjectId && IsParallelWorker())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("runtime catalog handles cannot be resolved in parallel workers")));
	if (handle < FirstCatalogViewObjectId || handle >= next_handle)
		return NULL;
	object = objects ? hash_search(objects, &handle, HASH_FIND, NULL) : NULL;
	if (!object)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("catalog object handle %u is no longer bound to a revision", handle)));
	return object;
}

/* Only these by-value SQL types can carry catalog object handles.  Never
 * interpret arbitrary scalar values or composite pointers as identifiers. */
bool
CatalogViewIsObjectIdType(Oid type)
{
	switch (type)
	{
		case OIDOID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGNAMESPACEOID:
			return true;
		default:
			return false;
	}
}

CatalogView *
CatalogViewObjectContext(Oid handle)
{
	CatalogViewObject *object = lookup_object(handle);

	return object ? object->view : NULL;
}

Oid
CatalogViewObjectStorage(Oid handle)
{
	CatalogViewObject *object = lookup_object(handle);

	return object ? object->storage : handle;
}

CatalogView *
CatalogViewSelectKeys(CatalogView *fallback, TupleDesc desc, int nkeys, const int *attributes, const Datum *values)
{
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		if (attributes[i] > 0 && TupleDescAttr(desc, attributes[i] - 1)->atttypid == OIDOID)
		{
			CatalogView *view = CatalogViewObjectContext(DatumGetObjectId(values[i]));

			if (view)
				return view;
		}
	}
	return fallback;
}

/* Sort the provider's tuples using the same operator family as the native index.
 * The index supplies ordering metadata only; its physical pages are never read.
 */
typedef struct CatalogViewOrder
{
	TupleDesc	desc;
	int			nkeys;
	AttrNumber *attributes;
	SortSupportData *support;
} CatalogViewOrder;

static int
compare_catalog_tuples(const void *left, const void *right, void *argument)
{
	CatalogViewOrder *order = argument;
	HeapTuple	a = *((HeapTuple const *) left);
	HeapTuple	b = *((HeapTuple const *) right);
	int			i;

	for (i = 0; i < order->nkeys; i++)
	{
		bool		anull,
					bnull;
		Datum		av = heap_getattr(a, order->attributes[i], order->desc, &anull);
		Datum		bv = heap_getattr(b, order->attributes[i], order->desc, &bnull);
		int			result = ApplySortComparator(av, anull, bv, bnull, &order->support[i]);

		if (result)
			return result;
	}
	return 0;
}

void
CatalogViewOrderScan(CatalogViewScan *scan, Relation index)
{
	CatalogViewOrder order;
	MemoryContext memory,
				previous;
	ListCell   *lc;
	int			i = 0;

	Assert(!scan->started && !scan->ordered);
	scan->ordered = palloc_array(HeapTuple, Max(list_length(scan->rows), 1));
	foreach(lc, scan->rows) scan->ordered[i++] = lfirst(lc);
	memory = AllocSetContextCreate(CurrentMemoryContext, "catalog scan ordering", ALLOCSET_SMALL_SIZES);
	previous = MemoryContextSwitchTo(memory);
	order.desc = RelationGetDescr(scan->relation);
	order.nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	order.attributes = index->rd_index->indkey.values;
	order.support = palloc0_array(SortSupportData, order.nkeys);
	for (i = 0; i < order.nkeys; i++)
	{
		SortSupport support = &order.support[i];

		if (order.attributes[i] <= 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("expression indexes cannot order a catalog view")));
		support->ssup_cxt = memory;
		support->ssup_attno = i + 1;
		support->ssup_collation = index->rd_indcollation[i];
		support->ssup_nulls_first = (index->rd_indoption[i] & INDOPTION_NULLS_FIRST) != 0;
		support->abbreviate = false;
		PrepareSortSupportFromIndexRel(index, (index->rd_indoption[i] & INDOPTION_DESC) != 0, support);
	}
	qsort_arg(scan->ordered, list_length(scan->rows), sizeof(HeapTuple), compare_catalog_tuples, &order);
	MemoryContextSwitchTo(previous);
	MemoryContextDelete(memory);
}

/* A provider tuple's synthetic TID must never be interpreted as a heap offset. */
void
CatalogViewRejectPhysicalAccess(Relation relation)
{
	if (CatalogViewContains(CatalogViewCurrent(), RelationGetRelid(relation)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("physical access to revision-bound catalog %s is not supported", RelationGetRelationName(relation))));
}

Oid
CatalogViewFindObject(CatalogView *view, Oid objectclass, const char *identity)
{
	ListCell   *lc;

	foreach(lc, view->handles)
	{
		Oid			handle = lfirst_oid(lc);
		CatalogViewObject *object = hash_search(objects, &handle, HASH_FIND, NULL);

		if (object->objectclass == objectclass && !strcmp(object->identity, identity))
			return handle;
	}
	return InvalidOid;
}

Oid
CatalogViewLookupObject(CatalogView *view, Oid objectclass, const char *identity)
{
	Oid			result = CatalogViewFindObject(view, objectclass, identity);

	if (OidIsValid(result))
		return result;
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("object %s does not exist in catalog context", identity)));
	return InvalidOid;
}

uint64
CatalogViewCacheId(CatalogView *view)
{
	return view ? view->serial : 0;
}

/* Select a single invocation scope for SQL and procedural handlers alike. */
CatalogView *
CatalogViewCallContext(FunctionCallInfo fcinfo)
{
	CatalogView *view = NULL;
	int			i;
	Oid		   *declared = NULL;
	int			nargs = 0;

	if (fcinfo->flinfo->fn_expr && IsA(fcinfo->flinfo->fn_expr, FuncExpr) &&
		((FuncExpr *) fcinfo->flinfo->fn_expr)->dolt_working)
		return NULL;
	view = CatalogViewObjectContext(fcinfo->flinfo->fn_oid);
	if (view)
		return view;

	for (i = 0; i < fcinfo->nargs; i++)
	{
		Oid			type = get_fn_expr_argtype(fcinfo->flinfo, i);
		CatalogView *bound;

		/* Internal fmgr callers need not supply an expression tree. */
		if (!OidIsValid(type))
		{
			if (!declared)
				(void) get_func_signature(fcinfo->flinfo->fn_oid, &declared, &nargs);
			if (i < nargs)
				type = declared[i];
		}
		bound = CatalogViewObjectContext(type);
		if (!fcinfo->args[i].isnull &&
			CatalogViewIsObjectIdType(type))
			bound = CatalogViewObjectContext(DatumGetObjectId(fcinfo->args[i].value));
		if (bound && view && view != bound)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SQL function arguments span incompatible catalog contexts")));
		if (bound)
			view = bound;
	}
	if (declared)
		pfree(declared);
	if (!view)
		view = CatalogViewCurrent();
	return view;
}

/* Search-path entries are session-global; translate them at the consumer. */
Oid
CatalogViewNamespace(CatalogView *view, Oid namespaceid)
{
	CatalogView *previous;
	char	   *name;
	Oid			result;

	if (!view || !OidIsValid(namespaceid) || CatalogViewObjectContext(namespaceid))
		return namespaceid;
	previous = CatalogViewSwitch(NULL);
	PG_TRY();
	{
		name = get_namespace_name(namespaceid);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	result = name ? CatalogViewFindObject(view, NamespaceRelationId, name) : InvalidOid;
	return OidIsValid(result) ? result : namespaceid;
}

/* Resolve an already selected runtime function handle, not a name in the
 * body's catalog scope. Shared helpers can execute over historical arguments. */
HeapTuple
CatalogViewFunctionTuple(Oid functionid)
{
	CatalogView *previous = CatalogViewSwitch(CatalogViewObjectContext(functionid));
	HeapTuple	result;

	PG_TRY();
	{
		result = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionid));
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

const char *
CatalogViewObjectIdentity(Oid handle, Oid objectclass)
{
	CatalogViewObject *object = lookup_object(handle);

	if (!object || object->objectclass != objectclass)
		elog(ERROR, "catalog handle %u does not identify class %u", handle, objectclass);
	return object->identity;
}

/* Native DDL may propose tuples, but only its owner may publish a new root. */
static CatalogViewMutation * catalog_mutation;

CatalogViewMutation *
CatalogViewSwitchMutation(CatalogViewMutation * mutation)
{
	CatalogViewMutation *previous = catalog_mutation;

	catalog_mutation = mutation;
	return previous;
}

bool
CatalogViewCaptureUpdate(Relation relation, HeapTuple tuple)
{
	if (!catalog_mutation || CatalogViewCurrent() != catalog_mutation->view ||
		!CatalogViewContains(catalog_mutation->view, RelationGetRelid(relation)))
		return false;
	catalog_mutation->update(relation, tuple, catalog_mutation->state);
	return true;
}

/* Keep handle identities for the backend lifetime, but bound decoded row/cache
 * residency. Plans and type caches may hold those handles after eviction. */
void
CatalogViewSetLoader(CatalogView *view, void (*loader) (CatalogView *, void *), void *arg)
{
	view->loader = loader;
	view->loader_arg = arg;
}

void
CatalogViewEnsureLoaded(CatalogView *view)
{
	view->touched = ++view_clock;
	if (CatalogViewCreatorAborted(view))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("catalog handle refers to an aborted schema revision")));
	if (view->loaded)
		return;
	if (!view->loader || view->loading)
		elog(ERROR, "catalog view cannot reload its rows");
	view->loading = true;
	PG_TRY();
	{
		view->loader(view, view->loader_arg);
		if (!view->loaded)
			elog(ERROR, "catalog view loader did not seal its rows");
	}
	PG_CATCH();
	{
		view->loading = false;
		view->loaded = false;
		view->catalogs = NIL;
		MemoryContextReset(view->rows_memory);
		PG_RE_THROW();
	}
	PG_END_TRY();
	view->loading = false;
}

static void
prune_catalog_views(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
		return;
	for (;;)
	{
		dlist_iter	it;
		CatalogView *oldest = NULL;
		int			count = 0;
		ListCell   *lc;
		List	   *kept = NIL;
		MemoryContext previous;

		dlist_foreach(it, &views)
		{
			CatalogView *view = dlist_container(CatalogView, link, it.cur);

			if (!view->loader || !view->loaded)
				continue;
			count++;
			if (view == current_view || view->scan_references || view->loading)
				continue;
			if (!oldest || view->touched < oldest->touched)
				oldest = view;
		}
		if (count <= catalog_view_cache_entries || !oldest)
			break;
		previous = MemoryContextSwitchTo(oldest->memory);
		foreach(lc, oldest->caches)
		{
			CatCache   *cache = lfirst(lc);

			/*
			 * Referenced cache tuples own their bytes. They remain valid but
			 * dead until their native resource owner releases them.
			 */
			if (!TryDestroyCatCache(cache))
				kept = lappend(kept, cache);
		}
		list_free(oldest->caches);
		oldest->caches = kept;
		oldest->catalogs = NIL;
		oldest->loaded = false;
		MemoryContextReset(oldest->rows_memory);
		MemoryContextSwitchTo(previous);
	}
}

/* Only provisional roots need this guard. Committed canonical objects are
 * retained; a backend handle must not make an aborted object visible. */
void
CatalogViewSetCreator(CatalogView *view, TransactionId creator)
{
	view->creator = creator;
}

bool
CatalogViewCreatorAborted(CatalogView *view)
{
	return TransactionIdIsValid(view->creator) &&
		!TransactionIdIsCurrentTransactionId(view->creator) &&
		!TransactionIdDidCommit(view->creator);
}
