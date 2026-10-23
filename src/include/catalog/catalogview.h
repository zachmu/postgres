/* Immutable revision-bound catalog views shared by native and SQL lookups. */
#ifndef CATALOGVIEW_H
#define CATALOGVIEW_H
#include "access/htup.h"
#include "fmgr.h"
#include "access/skey.h"
#include "access/sdir.h"
#include "access/tupdesc.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* Backend-local identifiers never enter WAL or canonical schema bytes. */
#define FirstCatalogViewObjectId ((Oid) 0xC0000000U)
struct catcache;
typedef struct CatalogView CatalogView;
typedef struct CatalogViewScan CatalogViewScan;

extern CatalogView *CatalogViewCreate(const char *identity);
extern void CatalogViewAddRows(CatalogView *view, Oid catalog, List *tuples);
extern void CatalogViewSeal(CatalogView *view);
extern int	catalog_view_cache_entries;
extern void CatalogViewSetLoader(CatalogView *view, void (*loader) (CatalogView *, void *), void *arg);
extern void CatalogViewEnsureLoaded(CatalogView *view);
extern void CatalogViewSetCreator(CatalogView *view, TransactionId creator);
extern bool CatalogViewCreatorAborted(CatalogView *view);
extern void CatalogViewRetain(CatalogView *view);
extern void CatalogViewRelease(CatalogView *view);
extern CatalogView *CatalogViewCurrent(void);
extern Oid	CatalogViewNamespace(CatalogView *view, Oid namespaceid);
extern uint64 CatalogViewCacheId(CatalogView *view);
extern Oid	CatalogViewBindObject(CatalogView *view, Oid objectclass, const char *identity, Oid storage);
extern Oid	CatalogViewFindObject(CatalogView *view, Oid objectclass, const char *identity);
extern Oid	CatalogViewLookupObject(CatalogView *view, Oid objectclass, const char *identity);
extern CatalogView *CatalogViewObjectContext(Oid handle);
extern bool CatalogViewIsObjectIdType(Oid type);
extern CatalogView *CatalogViewCallContext(FunctionCallInfo fcinfo);
extern HeapTuple CatalogViewFunctionTuple(Oid functionid);
extern Oid	CatalogViewObjectStorage(Oid handle);
extern const char *CatalogViewObjectIdentity(Oid handle, Oid objectclass);
extern CatalogView *CatalogViewSelectKeys(CatalogView *fallback, TupleDesc desc, int nkeys, const int *attributes, const Datum *values);

/* Callers must restore the previous value in PG_FINALLY, including on ERROR. */
extern CatalogView *CatalogViewSwitch(CatalogView *view);
extern bool CatalogViewContains(CatalogView *view, Oid catalog);
extern struct catcache *CatalogViewCache(CatalogView *view, struct catcache *base);
extern void CatalogViewInvalidateCache(int cacheid, uint32 hashvalue);
extern void CatalogViewRejectPhysicalAccess(Relation relation);

/* Private command-visible catalog mutation state for native DDL. */
extern bool CatalogViewCaptureUpdate(Relation relation, ItemPointer tid, HeapTuple tuple);
extern bool CatalogViewCaptureInsert(Relation relation, HeapTuple tuple);
extern bool CatalogViewCaptureDelete(Relation relation, ItemPointer tid);
extern void CatalogViewBeginMutation(CatalogView *view);
extern void CatalogViewSetMutationValidator(CatalogView *view, void (*validator) (Relation, HeapTuple));
extern bool CatalogViewIsProvisional(CatalogView *view);
extern Oid	CatalogViewNewObject(Oid catalog);
extern void CatalogViewMutationCommandEnd(void);
extern List *CatalogViewTuples(CatalogView *view, Oid catalog);
extern void CatalogViewRetireMutation(CatalogView *view);
extern CatalogViewScan *CatalogViewBeginScan(Relation relation, int nkeys, ScanKey keys);
extern void CatalogViewOrderScan(CatalogViewScan *scan, Relation index);
extern HeapTuple CatalogViewNext(CatalogViewScan *scan, ScanDirection direction);
extern void CatalogViewRescan(CatalogViewScan *scan, ScanKey keys);
extern void CatalogViewEndScan(CatalogViewScan *scan);
#endif
