/*
 * Canonical providers publish immutable catalog tuples through this boundary.
 * A view owns its rows and isolated native catcaches. Physical catalog rows are
 * not consulted when the view provides a catalog, including an empty catalog.
 */
#include "postgres.h"
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
	char	   *identity;
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

static CatalogView *current_view;
static dlist_head views = DLIST_STATIC_INIT(views);

CatalogView *
CatalogViewCreate(const char *identity)
{
	MemoryContext memory = AllocSetContextCreate(TopMemoryContext, "catalog view", ALLOCSET_DEFAULT_SIZES);
	MemoryContext previous = MemoryContextSwitchTo(memory);
	CatalogView *view = palloc0(sizeof(*view));

	view->memory = memory;
	view->identity = pstrdup(identity);
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

	if (view->sealed || CatalogViewContains(view, catalog))
		elog(ERROR, "cannot modify published catalog view");
	previous = MemoryContextSwitchTo(view->memory);
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
	current_view = view;
	return previous;
}

bool
CatalogViewContains(CatalogView *view, Oid catalog)
{
	ListCell   *lc;

	if (!view)
		return false;
	foreach(lc, view->catalogs)
		if (((CatalogViewRows *) lfirst(lc))->catalog == catalog)
		return true;
	return false;
}

CatCache *
CatalogViewCache(CatalogView *view, CatCache *base)
{
	ListCell   *lc;
	CatCache   *cache;
	MemoryContext previous;

	if (!CatalogViewContains(view, base->cc_reloid))
		return base;
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
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("catalog scan cannot follow this revision-bound handle")));
		return NULL;
	}
	scan = palloc0(sizeof(*scan));
	scan->view = view;
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

	if (handle < FirstCatalogViewObjectId || handle >= next_handle)
		return NULL;
	object = objects ? hash_search(objects, &handle, HASH_FIND, NULL) : NULL;
	if (!object)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("catalog object handle %u is no longer bound to a revision", handle)));
	return object;
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
		if (values[i] <= UINT32_MAX && attributes[i] > 0 && TupleDescAttr(desc, attributes[i] - 1)->atttypid == OIDOID)
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
