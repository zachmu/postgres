/* Native cache and scan boundary tests; no physical projection objects. */
#include "postgres.h"
#include "access/xact.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/catalogview.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type.h"
#include "utils/typcache.h"
#include "utils/resowner.h"
#include "access/relation.h"
#include "utils/lsyscache.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "utils/fmgroids.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(test_catalog_views);
PG_FUNCTION_INFO_V1(test_catalog_view_lifetime);

/* The fixture supplies decoded native rows directly, as a canonical provider
 * will. Physical template rows are only test data; no projected relation is
 * installed in any catalog. Both definitions deliberately use identical names. */
static void
add_composite(CatalogView *view, Oid namespace, Oid column_type,
			  Oid *relation_id, Oid *type_id)
{
	Oid			template = get_relname_relid("test_view_template", get_namespace_oid("public", false));
	Oid			template_type = get_rel_type_id(template);
	Oid			catalogs[] = {RelationRelationId, AttributeRelationId, TypeRelationId};
	int			c;

	*relation_id = CatalogViewBindObject(view, RelationRelationId, "same_table", InvalidOid);
	*type_id = CatalogViewBindObject(view, TypeRelationId, "same_table", InvalidOid);
	for (c = 0; c < lengthof(catalogs); c++)
	{
		Relation	rel = table_open(catalogs[c], AccessShareLock);
		TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
		HeapTuple	tuple;
		List	   *rows = NIL;

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			rows = lappend(rows, heap_copytuple(tuple));
			if (catalogs[c] == RelationRelationId && ((Form_pg_class) GETSTRUCT(tuple))->oid == template)
			{
				HeapTuple	copy = heap_copytuple(tuple);
				Form_pg_class form = (Form_pg_class) GETSTRUCT(copy);

				form->oid = *relation_id;
				form->reltype = *type_id;
				form->relnamespace = namespace;
				namestrcpy(&form->relname, "same_table");
				rows = lappend(rows, copy);
			}
			else if (catalogs[c] == AttributeRelationId && ((Form_pg_attribute) GETSTRUCT(tuple))->attrelid == template)
			{
				HeapTuple	copy = heap_copytuple(tuple);
				Form_pg_attribute form = (Form_pg_attribute) GETSTRUCT(copy);

				form->attrelid = *relation_id;
				if (form->attnum == 1)
				{
					HeapTuple	type = SearchSysCache1(TYPEOID, ObjectIdGetDatum(column_type));
					Form_pg_type typ = (Form_pg_type) GETSTRUCT(type);

					form->atttypid = column_type;
					form->attlen = typ->typlen;
					form->attbyval = typ->typbyval;
					form->attalign = typ->typalign;
					form->attstorage = typ->typstorage;
					form->attcollation = typ->typcollation;
					ReleaseSysCache(type);
				}
				rows = lappend(rows, copy);
			}
			else if (catalogs[c] == TypeRelationId && ((Form_pg_type) GETSTRUCT(tuple))->oid == template_type)
			{
				HeapTuple	copy = heap_copytuple(tuple);
				Form_pg_type form = (Form_pg_type) GETSTRUCT(copy);

				form->oid = *type_id;
				form->typrelid = *relation_id;
				form->typnamespace = namespace;
				form->typarray = InvalidOid;
				namestrcpy(&form->typname, "same_table");
				rows = lappend(rows, copy);
			}
		}
		table_endscan(scan);
		CatalogViewAddRows(view, catalogs[c], rows);
		list_free_deep(rows);
		table_close(rel, AccessShareLock);
	}
}

static CatalogView *
make_view(const char *identity, Oid *oid, Oid column_type, Oid *relation_id, Oid *type_id)
{
	CatalogView *view = CatalogViewCreate(identity);
	Relation	rel = table_open(NamespaceRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
	HeapTuple	tuple;
	List	   *rows = NIL;
	Datum		values[Natts_pg_namespace] = {0};
	bool		nulls[Natts_pg_namespace] = {false};
	NameData	name;

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		rows = lappend(rows, heap_copytuple(tuple));
	table_endscan(scan);
	namestrcpy(&name, "view_example");
	*oid = CatalogViewBindObject(view, NamespaceRelationId, "view_example", InvalidOid);
	values[Anum_pg_namespace_oid - 1] = ObjectIdGetDatum(*oid);
	values[Anum_pg_namespace_nspname - 1] = NameGetDatum(&name);
	values[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(10);
	nulls[Anum_pg_namespace_nspacl - 1] = true;
	rows = lappend(rows, heap_form_tuple(RelationGetDescr(rel), values, nulls));
	CatalogViewAddRows(view, NamespaceRelationId, rows);
	if (relation_id)
		add_composite(view, *oid, column_type, relation_id, type_id);
	CatalogViewSeal(view);
	table_close(rel, AccessShareLock);
	return view;
}

static void
check_write_rejection(Relation rel)
{
	volatile bool rejected = false;
	MemoryContext saved = CurrentMemoryContext;
	ItemPointerData synthetic;

	ItemPointerSet(&synthetic, 1, 1);
	PG_TRY();
	{
		CatalogTupleDelete(rel, &synthetic);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved);
		if (geterrcode() != ERRCODE_FEATURE_NOT_SUPPORTED)
			PG_RE_THROW();
		FlushErrorState();
		rejected = true;
	}
	PG_END_TRY();
	if (!rejected)
		elog(ERROR, "catalog write bypassed provider");
}

Datum
test_catalog_views(PG_FUNCTION_ARGS)
{
	Oid			aid,
				bid,
				arel,
				brel,
				atype,
				btype;
	CatalogView *a = make_view("a", &aid, INT4OID, &arel, &atype);
	CatalogView *b = make_view("b", &bid, TEXTOID, &brel, &btype);
	CatalogView *previous = CatalogViewCurrent();
	int			pass;

	PG_TRY();
	{
		/* Repeat after both positive and negative entries have been cached. */
		for (pass = 0; pass < 6; pass++)
		{
			CatalogView *view = pass % 2 ? b : a;
			Oid			wanted = pass % 2 ? bid : aid;
			HeapTuple	tuple;
			Relation	rel;
			SysScanDesc scan;
			ScanKeyData key;
			bool		isnull;

			CatalogViewSwitch(view);
			{
				/*
				 * Deliberately open the other view's relation and composite
				 * type.
				 */
				Oid			other_relation = pass % 2 ? arel : brel;
				Oid			other_type = pass % 2 ? atype : btype;
				Oid			expected = pass % 2 ? INT4OID : TEXTOID;
				Relation	composite = relation_open(other_relation, AccessShareLock);
				TypeCacheEntry *type = lookup_type_cache(other_type, TYPECACHE_TUPDESC);

				if (TupleDescAttr(RelationGetDescr(composite), 0)->atttypid != expected ||
					!type->tupDesc || TupleDescAttr(type->tupDesc, 0)->atttypid != expected)
					elog(ERROR, "relation or composite type crossed catalog contexts");
				relation_close(composite, AccessShareLock);
			}
			tuple = SearchSysCache1(NAMESPACENAME, CStringGetDatum("view_example"));
			if (!tuple || ((Form_pg_namespace) GETSTRUCT(tuple))->oid != wanted)
				elog(ERROR, "namespace cache crossed catalog contexts");
			ReleaseSysCache(tuple);
			tuple = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(pass % 2 ? aid : bid));
			if (!tuple || ((Form_pg_namespace) GETSTRUCT(tuple))->oid != (pass % 2 ? aid : bid))
				elog(ERROR, "bound OID did not select its own context");
			ReleaseSysCache(tuple);
			if (SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("absent_view_namespace")))
				elog(ERROR, "negative catalog cache returned a tuple");
			rel = table_open(NamespaceRelationId, AccessShareLock);
			ScanKeyInit(&key, Anum_pg_namespace_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(wanted));
			scan = systable_beginscan(rel, NamespaceOidIndexId, true, NULL, 1, &key);
			tuple = systable_getnext(scan);
			if (!tuple || ((Form_pg_namespace) GETSTRUCT(tuple))->oid != wanted || systable_getnext(scan))
				elog(ERROR, "native catalog scan disagrees with cache");
			systable_endscan(scan);
			check_write_rejection(rel);
			{
				Relation	index = index_open(NamespaceOidIndexId, AccessShareLock);
				Oid			last = InvalidOid;
				int			count = 0;

				scan = systable_beginscan_ordered(rel, index, NULL, 0, NULL);
				while ((tuple = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
				{
					Oid			current = ((Form_pg_namespace) GETSTRUCT(tuple))->oid;

					if (current <= last)
						elog(ERROR, "catalog view index order is incorrect");
					last = current;
					count++;
				}
				if (last != wanted || count < 2)
					elog(ERROR, "ordered scan omitted view rows");
				systable_endscan_ordered(scan);
				scan = systable_beginscan_ordered(rel, index, NULL, 1, &key);
				tuple = systable_getnext_ordered(scan, BackwardScanDirection);
				if (!tuple || ((Form_pg_namespace) GETSTRUCT(tuple))->oid != wanted || systable_getnext_ordered(scan, BackwardScanDirection))
					elog(ERROR, "backward ordered scan disagrees with cache");
				systable_endscan_ordered(scan);
				index_close(index, AccessShareLock);
			}
			table_close(rel, AccessShareLock);
			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI connect failed");
			SPI_execute("SELECT oid FROM pg_catalog.pg_namespace WHERE nspname='view_example'", true, 0);
			if (SPI_processed != 1 || DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull)) != wanted)
				elog(ERROR, "SQL catalog scan disagrees with cache");
			SPI_finish();
		}
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	CatalogViewRelease(a);
	CatalogViewRelease(b);
	if (SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("view_example")))
		elog(ERROR, "view namespace leaked into physical catalog");
	PG_RETURN_BOOL(true);
}

/* A borrowed cache tuple pins its context independently of the provider owner. */
Datum
test_catalog_view_lifetime(PG_FUNCTION_ARGS)
{
	Oid			id;
	CatalogView *view = make_view("pin", &id, InvalidOid, NULL, NULL);
	CatalogView *previous = CatalogViewSwitch(view);
	HeapTuple	held;
	HeapTuple	again;
	volatile bool rejected = false;
	MemoryContext memory = CurrentMemoryContext;

	PG_TRY();
	{
		held = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(id));
		if (!HeapTupleIsValid(held))
			elog(ERROR, "missing provider tuple");
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	CatalogViewRelease(view);
	/* Only the borrowed tuple now keeps the view alive. */
	again = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(id));
	if (!HeapTupleIsValid(again) || ((Form_pg_namespace) GETSTRUCT(again))->oid != id)
		elog(ERROR, "borrowed cache entry failed to pin context");
	ReleaseSysCache(again);
	ReleaseSysCache(held);
	PG_TRY();
	{
		(void) CatalogViewObjectContext(id);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(memory);
		if (geterrcode() != ERRCODE_UNDEFINED_OBJECT)
			PG_RE_THROW();
		FlushErrorState();
		rejected = true;
	}
	PG_END_TRY();
	if (!rejected)
		elog(ERROR, "retired handle did not fail closed");
	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(test_catalog_mutation);
Datum
test_catalog_mutation(PG_FUNCTION_ARGS)
{
	Oid			id,
				inserted;
	CatalogView *view = make_view("mutation", &id, InvalidOid, NULL, NULL);
	CatalogView *previous;
	Relation	relation;
	HeapTuple	old,
				changed,
				added;
	CatalogViewScan *scan;
	CatalogViewScan *retained = NULL;
	bool		found_old = false;

	CatalogViewBeginMutation(view);
	previous = CatalogViewSwitch(view);

	PG_TRY();
	{
		relation = table_open(NamespaceRelationId, RowExclusiveLock);
		old = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(id));
		changed = heap_copytuple(old);
		namestrcpy(&((Form_pg_namespace) GETSTRUCT(changed))->nspname, "view_changed");
		scan = CatalogViewBeginScan(relation, 0, NULL);
		CatalogTupleUpdate(relation, &old->t_self, changed);
		if (!SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("view_example")))
			elog(ERROR, "unpublished mutation changed current command");
		CommandCounterIncrement();
		if (!SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("view_changed")) ||
			SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("view_example")))
			elog(ERROR, "CCI failed to refresh staged namespace keys");
		if (strcmp(NameStr(((Form_pg_namespace) GETSTRUCT(old))->nspname), "view_example"))
			elog(ERROR, "borrowed tuple changed in place");
		while ((added = CatalogViewNext(scan, ForwardScanDirection)) != NULL)
			if (!strcmp(NameStr(((Form_pg_namespace) GETSTRUCT(added))->nspname), "view_example"))
				found_old = true;
		if (!found_old)
			elog(ERROR, "old scan lost its command-visible tuple version");
		CatalogViewEndScan(scan);
		ReleaseSysCache(old);
		inserted = GetNewOidWithIndex(relation, NamespaceOidIndexId, Anum_pg_namespace_oid);
		added = heap_copytuple(changed);
		((Form_pg_namespace) GETSTRUCT(added))->oid = inserted;
		namestrcpy(&((Form_pg_namespace) GETSTRUCT(added))->nspname, "new_namespace");
		CatalogTupleInsert(relation, added);
		CommandCounterIncrement();
		if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(inserted)))
			elog(ERROR, "staged insert is invisible");
		{
			MemoryContext saved_memory = CurrentMemoryContext;
			ResourceOwner saved_owner = CurrentResourceOwner;
			volatile bool rejected = false;

			BeginInternalSubTransaction(NULL);
			PG_TRY();
			{
				HeapTuple	duplicate = heap_copytuple(added);

				((Form_pg_namespace) GETSTRUCT(duplicate))->oid = GetNewOidWithIndex(relation, NamespaceOidIndexId, Anum_pg_namespace_oid);
				CatalogTupleInsert(relation, duplicate);
				ReleaseCurrentSubTransaction();
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(saved_memory);
				if (geterrcode() != ERRCODE_UNIQUE_VIOLATION)
					PG_RE_THROW();
				FlushErrorState();
				RollbackAndReleaseCurrentSubTransaction();
				rejected = true;
			}
			PG_END_TRY();
			MemoryContextSwitchTo(saved_memory);
			CurrentResourceOwner = saved_owner;
			if (!rejected)
				elog(ERROR, "provisional unique namespace name was not enforced");
			rejected = false;
			BeginInternalSubTransaction(NULL);
			PG_TRY(nested);
			{
				if (SPI_connect() != SPI_OK_CONNECT)
					elog(ERROR, "cannot connect mutation test SPI");
				SPI_execute("SET application_name='unpublished utility'", false, 0);
				SPI_finish();
				ReleaseCurrentSubTransaction();
			}
			PG_CATCH(nested);
			{
				MemoryContextSwitchTo(saved_memory);
				if (geterrcode() != ERRCODE_FEATURE_NOT_SUPPORTED)
					PG_RE_THROW();
				FlushErrorState();
				RollbackAndReleaseCurrentSubTransaction();
				rejected = true;
			}
			PG_END_TRY(nested);
			MemoryContextSwitchTo(saved_memory);
			CurrentResourceOwner = saved_owner;
			if (!rejected)
				elog(ERROR, "nested utility escaped the mutation publication contract");
			BeginInternalSubTransaction(NULL);
			{
				HeapTuple	child = heap_copytuple(added);

				namestrcpy(&((Form_pg_namespace) GETSTRUCT(child))->nspname, "child_namespace");
				CatalogTupleUpdate(relation, &added->t_self, child);
				CommandCounterIncrement();
				if (!SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("child_namespace")))
					elog(ERROR, "subtransaction mutation is invisible");
				RollbackAndReleaseCurrentSubTransaction();
			}
			MemoryContextSwitchTo(saved_memory);
			CurrentResourceOwner = saved_owner;
			if (!SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("new_namespace")) ||
				SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("child_namespace")))
				elog(ERROR, "subtransaction rollback failed to restore parent catalog state");
		}
		CatalogTupleDelete(relation, &added->t_self);
		CommandCounterIncrement();
		if (SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(inserted)))
			elog(ERROR, "staged delete is still visible");
		heap_freetuple(added);
		heap_freetuple(changed);
		retained = CatalogViewBeginScan(relation, 0, NULL);
		table_close(relation, RowExclusiveLock);
	}
	PG_FINALLY();
	{
		CatalogViewSwitch(previous);
		CatalogViewRetireMutation(view);
	}
	PG_END_TRY();
	if (!HeapTupleIsValid(CatalogViewNext(retained, ForwardScanDirection)))
		elog(ERROR, "retirement invalidated an already pinned catalog scan");
	CatalogViewEndScan(retained);
	CatalogViewRelease(view);
	if (SearchSysCacheExists1(NAMESPACENAME, CStringGetDatum("view_changed")))
		elog(ERROR, "provisional namespace leaked into physical catalog");
	PG_RETURN_BOOL(true);
}
