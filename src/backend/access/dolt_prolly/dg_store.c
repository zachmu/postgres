/*-------------------------------------------------------------------------
 *
 * dg_store.c
 *	  Prolly relation state: trees in the chunk store, roots in pg_dolt_root.
 *
 * Every tree node lives in the chunk store (access/doltchunk.h). A relation's
 * committed roots, per branch, live in one pg_dolt_root row, so a snapshot
 * selects them with ordinary MVCC. Relation files hold no prolly data.
 *
 * Within a transaction, nodes are built and read through a working store that
 * loads missing nodes from the chunk store on demand. Saving a state writes
 * nothing: it pushes the new roots onto the transaction's overlay, a stack of
 * (command ID, subtransaction, base roots, roots) per relation storage and
 * branch. Loads consult the overlay first, so a statement sees its earlier
 * statements' writes by command ID, and a subtransaction rollback pops its
 * entries. If an entry's base differs from the committed roots a snapshot
 * selects, the view is the three-way merge of the two.
 *
 * Just before commit or PREPARE, each written (storage, branch) publishes: it
 * locks or creates its root row, merges its changes onto the latest committed
 * roots, writes new nodes to the chunk store and stores the merged roots. The
 * row lock is held until commit, after the transaction becomes visible, so a
 * snapshot that sees one publisher sees every earlier one.
 *
 * Handles, which serve as TIDs, come from a per-storage counter in shared
 * memory and are never reused. After a restart the counter starts above the
 * largest handle in any root row version that did not abort.
 *
 * src/backend/access/dolt_prolly/dg_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/doltchunk.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalogview.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_root.h"
#include "access/doltbranch.h"
#include "access/doltcatalog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "common/hashfn.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

void
dg_unsupported(const char *operation)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("prolly phase 2 does not support %s", operation)));
	pg_unreachable();
}
void
dg_check(PLStatus status)
{
	if (status != PL_OK)
		ereport(ERROR, (errmsg("prolly storage: %s", pl_status_string(status))));
}

/* ---------------------------------------------------------------------------
 * Roots
 * ---------------------------------------------------------------------------
 */

/* Which committed roots: one relation storage on one branch. */
typedef struct RootKey
{
	Oid			space;
	RelFileNumber file;
	char		branch[NAMEDATALEN * 2];
} RootKey;

typedef struct Roots
{
	bool		present;
	bool		table;			/* has handle and key trees */
	PLHash		content;
	PLHash		handles;
	PLHash		keys;
} Roots;

static bool
roots_equal(const Roots *a, const Roots *b)
{
	if (a->present != b->present)
		return false;
	if (!a->present)
		return true;
	return pl_hash_equal(a->content, b->content) && a->table == b->table &&
		(!a->table || (pl_hash_equal(a->handles, b->handles) && pl_hash_equal(a->keys, b->keys)));
}

static RootKey
root_key(Relation rel, const char *branch)
{
	RootKey		key = {0};

	if (strlen(branch) >= sizeof(key.branch))
		ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("branch name is too long for prolly storage")));
	key.space = rel->rd_locator.spcOid;
	key.file = rel->rd_locator.relNumber;
	strlcpy(key.branch, branch, sizeof(key.branch));
	return key;
}

static void
root_scan_keys(const RootKey *key, ScanKeyData *keys)
{
	ScanKeyInit(&keys[0], Anum_pg_dolt_root_rootspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_root_rootfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_root_rootbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(key->branch));
}

static void
hash_column(HeapTuple tuple, TupleDesc desc, AttrNumber attribute, bool *present, PLHash *hash)
{
	bool		isnull;
	Datum		value = heap_getattr(tuple, attribute, desc, &isnull);
	bytea	   *bytes;

	*present = !isnull;
	if (isnull)
		return;
	bytes = DatumGetByteaPP(value);
	if (VARSIZE_ANY_EXHDR(bytes) != PL_HASH_SIZE)
		elog(ERROR, "invalid prolly root hash");
	memcpy(hash->bytes, VARDATA_ANY(bytes), PL_HASH_SIZE);
}

static Roots
roots_from_tuple(HeapTuple tuple, TupleDesc desc)
{
	Roots		roots = {.present = true};
	bool		present,
				keys;

	hash_column(tuple, desc, Anum_pg_dolt_root_rootcontent, &present, &roots.content);
	hash_column(tuple, desc, Anum_pg_dolt_root_roothandles, &roots.table, &roots.handles);
	hash_column(tuple, desc, Anum_pg_dolt_root_rootkeys, &keys, &roots.keys);
	if (!present || roots.table != keys)
		elog(ERROR, "invalid prolly root row");
	return roots;
}

/* The committed roots |snapshot| selects. */
static Roots
roots_read(const RootKey *key, Snapshot snapshot)
{
	Relation	catalog = table_open(DoltRootRelationId, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Roots		roots = {0};

	/* Callers such as foreign key checks may pass an unregistered snapshot. */
	snapshot = RegisterSnapshot(snapshot);
	root_scan_keys(key, keys);
	scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, snapshot, 3, keys);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		roots = roots_from_tuple(tuple, RelationGetDescr(catalog));
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(catalog, AccessShareLock);
	return roots;
}

/* The latest committed roots, whatever the caller's snapshot. */
static Roots
roots_latest(const RootKey *key)
{
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	Roots		roots = roots_read(key, snapshot);

	UnregisterSnapshot(snapshot);
	return roots;
}

/* ---------------------------------------------------------------------------
 * The transaction's working store and chunk store handle
 * ---------------------------------------------------------------------------
 */

static PLStore *working_store;
static DoltChunkStore *working_chunks;
static DoltChunk working_fetched;

/* Open the read handle under the top transaction, so subtransaction rollback
 * cannot close it underneath the working store. */
static DoltChunkStore *
working_chunk_store(void)
{
	if (!working_chunks)
	{
		ResourceOwner owner = CurrentResourceOwner;
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		CurrentResourceOwner = TopTransactionResourceOwner;
		PG_TRY();
		{
			working_chunks = dolt_chunk_open(DOLT_CHUNK_READ);
		}
		PG_FINALLY();
		{
			CurrentResourceOwner = owner;
			MemoryContextSwitchTo(old);
		}
		PG_END_TRY();
	}
	return working_chunks;
}

static PLStatus
fetch_chunk(void *arg, PLHash hash, PLSlice *bytes)
{
	DoltChunkStore *chunks = working_chunk_store();
	ResourceOwner owner = CurrentResourceOwner;
	bool		found;

	/* The store copies each node before the next fetch. */
	CurrentResourceOwner = TopTransactionResourceOwner;
	PG_TRY();
	{
		if (working_fetched.pin)
			dolt_chunk_release(chunks, &working_fetched);
		found = dolt_chunk_get(chunks, hash.bytes, &working_fetched);
	}
	PG_FINALLY();
	{
		CurrentResourceOwner = owner;
	}
	PG_END_TRY();
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly chunk is missing from the chunk store")));
	bytes->data = working_fetched.data;
	bytes->len = working_fetched.length;
	return PL_OK;
}

PLStore *
dg_working_store(void)
{
	if (!working_store)
	{
		working_store = pl_store_create();
		if (!working_store)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory creating prolly store")));
		pl_store_set_fetch(working_store, fetch_chunk, NULL);
	}
	return working_store;
}

static void
working_close(bool commit)
{
	if (working_chunks)
	{
		if (commit)
		{
			ResourceOwner owner = CurrentResourceOwner;

			/* The handle and its pins belong to the top transaction. */
			CurrentResourceOwner = TopTransactionResourceOwner;
			if (working_fetched.pin)
				dolt_chunk_release(working_chunks, &working_fetched);
			dolt_chunk_close(working_chunks);
			CurrentResourceOwner = owner;
		}
		working_chunks = NULL;
	}
	memset(&working_fetched, 0, sizeof(working_fetched));
}

static PLTree
tree_at(PLHash root)
{
	PLTree		tree = {dg_working_store(), root};

	return tree;
}

static PLTree
empty_tree(void)
{
	PLTree		tree;

	dg_check(pl_build(dg_working_store(), NULL, 0, &tree));
	return tree;
}

/*
 * Write every node reachable from |root| that is not yet in the chunk store.
 * A node absent from the working store was never built or imported here, so it
 * came from the chunk store already; a fetched node is marked backed.
 */
typedef struct Pending
{
	PLNode	  **nodes;
	int			count;
	int			capacity;
} Pending;

static void
collect_unbacked(PLStore *store, PLHash root, Pending *pending)
{
	PLNode	   *node = pl_find_local(store, root);

	if (!node || node->backed)
		return;
	check_stack_depth();
	for (uint32 i = 0; node->level && i < node->count; i++)
	{
		PLHash		child;

		memcpy(child.bytes, pl_node_value(node, i).data, PL_HASH_SIZE);
		collect_unbacked(store, child, pending);
	}
	if (node->backed)
		return;					/* reached twice through shared subtrees */
	node->backed = true;
	if (pending->count == pending->capacity)
	{
		pending->capacity = pending->capacity ? pending->capacity * 2 : 64;
		pending->nodes = pending->nodes ? repalloc(pending->nodes, pending->capacity * sizeof(PLNode *)) :
			palloc(pending->capacity * sizeof(PLNode *));
	}
	pending->nodes[pending->count++] = node;
}

static void
persist_roots(const Roots *roots)
{
	PLStore    *store = dg_working_store();
	Pending		pending = {0};

	if (!roots->present)
		return;
	collect_unbacked(store, roots->content, &pending);
	if (roots->table)
	{
		collect_unbacked(store, roots->handles, &pending);
		collect_unbacked(store, roots->keys, &pending);
	}
	if (pending.count)
	{
		DoltChunkStore *chunks = dolt_chunk_open(DOLT_CHUNK_WRITE);
		uint8	   *hashes = palloc((Size) pending.count * DOLT_CHUNK_HASH_SIZE);
		const uint8 **data = palloc(pending.count * sizeof(uint8 *));
		Size	   *lengths = palloc(pending.count * sizeof(Size));

		for (int i = 0; i < pending.count; i++)
		{
			memcpy(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, pending.nodes[i]->hash.bytes, DOLT_CHUNK_HASH_SIZE);
			data[i] = pending.nodes[i]->data;
			lengths[i] = pending.nodes[i]->size;
		}
		dolt_chunk_put_many(chunks, pending.count, hashes, data, lengths);
		dolt_chunk_close(chunks);
		pfree(hashes);
		pfree(data);
		pfree(lengths);
		pfree(pending.nodes);
	}
}

/* ---------------------------------------------------------------------------
 * Three-way merge of roots
 * ---------------------------------------------------------------------------
 */

/*
 * Content and index values are an occurrence count followed by the row, so
 * concurrent changes to the same key combine additively.
 */
static PLStatus
merge_counts(void *arg, PLSlice key, PLValue base, PLValue left, PLValue right, PLValue *result)
{
	int64		b = base.present ? (int64) pl_read64(base.bytes.data) : 0;
	int64		l = left.present ? (int64) pl_read64(left.bytes.data) : 0;
	int64		r = right.present ? (int64) pl_read64(right.bytes.data) : 0;
	int64		count = r + l - b;
	PLSlice		row = left.present ? left.bytes : right.present ? right.bytes : base.bytes;
	uint8	   *bytes;

	if (count < 0)
		return PL_CONFLICT;
	if (count == 0)
	{
		result->present = false;
		return PL_OK;
	}
	bytes = palloc(row.len);
	memcpy(bytes, row.data, row.len);
	pl_write64(bytes, (uint64) count);
	result->present = true;
	result->bytes.data = bytes;
	result->bytes.len = row.len;
	return PL_OK;
}

static PLHash
merge_tree(PLHash base, PLHash left, PLHash right, bool counts)
{
	PLTree		b = tree_at(base),
				l = tree_at(left),
				r = tree_at(right),
				out;
	PLStatus	status = pl_merge(&b, &l, &r, counts ? merge_counts : NULL, NULL, &out);

	if (status == PL_CONFLICT)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to concurrent update"),
				 errdetail("Concurrent changes to the same prolly rows could not be combined.")));
	dg_check(status);
	return out.root;
}

/* Combine changes made on |base| (ours) with the committed |theirs|. */
static Roots
roots_merge(const Roots *base, const Roots *ours, const Roots *theirs)
{
	Roots		empty = {0},
				result = *ours;
	Roots		b = *base;

	if (roots_equal(base, theirs))
		return *ours;
	if (roots_equal(base, ours))
		return *theirs;
	if (!ours->present)
		return *ours;
	if (!b.present)
	{
		PLTree		e = empty_tree();

		b.present = true;
		b.table = ours->table;
		b.content = b.handles = b.keys = e.root;
	}
	if (!theirs->present)
	{
		/* Nothing committed yet: only our changes exist. */
		empty = b;
		theirs = &empty;
	}
	result.content = merge_tree(b.content, ours->content, theirs->content, true);
	if (ours->table)
	{
		result.handles = merge_tree(b.handles, ours->handles, theirs->handles, false);
		result.keys = merge_tree(b.keys, ours->keys, theirs->keys, false);
	}
	return result;
}

/* ---------------------------------------------------------------------------
 * The transaction overlay
 * ---------------------------------------------------------------------------
 */

typedef struct OverlayEntry
{
	CommandId	cid;
	SubTransactionId subxid;
	Roots		base;			/* committed roots the entry was built on */
	Roots		roots;
} OverlayEntry;

typedef struct Overlay
{
	RootKey		key;			/* hash key */
	OverlayEntry *entries;
	int			count;
	int			capacity;
} Overlay;

static HTAB *overlays;
static bool callbacks_registered;

/*
 * Rows this transaction updated or deleted, by handle, with the command and
 * subtransaction that did it. A row changed by the current command must be
 * skipped rather than changed again, as PostgreSQL does for heap tuples.
 */
typedef struct ModifiedKey
{
	RootKey		root;
	uint64		handle;
} ModifiedKey;

typedef struct Modified
{
	ModifiedKey key;
	CommandId	cid;
	SubTransactionId subxid;
} Modified;

static HTAB *modified;

/*
 * Every edit builds new nodes and orphans old ones, and the working store keeps
 * all of them until the transaction ends. Once it grows past a threshold,
 * collection frees the nodes no live state or overlay entry can reach. Live
 * states register here, because scans hold slices borrowed from their nodes.
 */
#define DG_COLLECT_MINIMUM ((Size) 64 * 1024 * 1024)

static DGState **live_states;
static int	live_count;
static int	live_capacity;
static Size collect_threshold = DG_COLLECT_MINIMUM;


static void store_xact(XactEvent event, void *arg);
static void store_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg);

static Overlay *
overlay_find(const RootKey *key, bool create)
{
	bool		found;
	Overlay    *overlay;

	if (!overlays)
	{
		HASHCTL		ctl = {0};

		if (!create)
			return NULL;
		ctl.keysize = sizeof(RootKey);
		ctl.entrysize = sizeof(Overlay);
		ctl.hcxt = TopTransactionContext;
		overlays = hash_create("prolly transaction overlays", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	overlay = hash_search(overlays, key, create ? HASH_ENTER : HASH_FIND, &found);
	if (overlay && !found)
	{
		overlay->entries = NULL;
		overlay->count = 0;
		overlay->capacity = 0;
	}
	return overlay;
}

/* The entry |snapshot| sees: the latest, or the latest before its command. */
static OverlayEntry *
overlay_entry(Overlay *overlay, Snapshot snapshot)
{
	if (!overlay || !overlay->count)
		return NULL;
	if (!snapshot || !IsMVCCSnapshot(snapshot))
		return &overlay->entries[overlay->count - 1];
	for (int i = overlay->count - 1; i >= 0; i--)
		if (overlay->entries[i].cid < snapshot->curcid)
			return &overlay->entries[i];
	return NULL;
}

static void
overlay_push(const RootKey *key, CommandId cid, const Roots *base, const Roots *roots)
{
	Overlay    *overlay = overlay_find(key, true);
	OverlayEntry *top;

	if (overlay->count && overlay->entries[overlay->count - 1].cid == cid &&
		overlay->entries[overlay->count - 1].subxid == GetCurrentSubTransactionId())
		top = &overlay->entries[overlay->count - 1];
	else
	{
		if (overlay->count == overlay->capacity)
		{
			overlay->capacity = overlay->capacity ? overlay->capacity * 2 : 4;
			overlay->entries = overlay->entries ?
				repalloc(overlay->entries, overlay->capacity * sizeof(OverlayEntry)) :
				MemoryContextAlloc(TopTransactionContext, overlay->capacity * sizeof(OverlayEntry));
		}
		top = &overlay->entries[overlay->count++];
	}
	top->cid = cid;
	top->subxid = GetCurrentSubTransactionId();
	top->base = *base;
	top->roots = *roots;
}

static int
compare_overlays(const void *a, const void *b)
{
	const Overlay *x = *(Overlay *const *) a;
	const Overlay *y = *(Overlay *const *) b;

	if (x->key.space != y->key.space)
		return x->key.space < y->key.space ? -1 : 1;
	if (x->key.file != y->key.file)
		return x->key.file < y->key.file ? -1 : 1;
	return strcmp(x->key.branch, y->key.branch);
}

/*
 * Lock the root row for |key|, waiting out a concurrent publisher, and return
 * the latest committed roots. Creates no row; *tid is invalid if none exists.
 */
static Roots
roots_lock(Relation catalog, const RootKey *key, ItemPointer tid)
{
	for (;;)
	{
		Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
		ScanKeyData keys[3];
		SysScanDesc scan;
		HeapTuple	tuple;
		Roots		roots = {0};
		TM_Result	result = TM_Ok;

		root_scan_keys(key, keys);
		scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, snapshot, 3, keys);
		tuple = systable_getnext(scan);
		ItemPointerSetInvalid(tid);
		if (HeapTupleIsValid(tuple))
		{
			HeapTupleData locked;
			Buffer		buffer = InvalidBuffer;
			TM_FailureData failure;

			roots = roots_from_tuple(tuple, RelationGetDescr(catalog));
			locked.t_self = tuple->t_self;
			result = heap_lock_tuple(catalog, &locked, GetCurrentCommandId(true), LockTupleExclusive,
									 LockWaitBlock, false, &buffer, &failure);
			if (BufferIsValid(buffer))
				ReleaseBuffer(buffer);
			*tid = tuple->t_self;
		}
		systable_endscan(scan);
		UnregisterSnapshot(snapshot);
		if (result == TM_Ok || result == TM_SelfModified)
			return roots;

		/* Another publisher committed first; read its roots and retry. */
	}
}

static void
roots_write(Relation catalog, const RootKey *key, ItemPointer tid, const Roots *roots)
{
	Datum		values[Natts_pg_dolt_root];
	bool		nulls[Natts_pg_dolt_root] = {false};
	bytea	   *hashes[3];
	PLHash		sources[3] = {roots->content, roots->handles, roots->keys};
	HeapTuple	tuple;

	for (int i = 0; i < 3; i++)
	{
		hashes[i] = palloc(VARHDRSZ + PL_HASH_SIZE);
		SET_VARSIZE(hashes[i], VARHDRSZ + PL_HASH_SIZE);
		memcpy(VARDATA(hashes[i]), sources[i].bytes, PL_HASH_SIZE);
	}
	values[Anum_pg_dolt_root_rootspace - 1] = ObjectIdGetDatum(key->space);
	values[Anum_pg_dolt_root_rootfile - 1] = ObjectIdGetDatum(key->file);
	values[Anum_pg_dolt_root_rootbranch - 1] = CStringGetTextDatum(key->branch);
	values[Anum_pg_dolt_root_rootcontent - 1] = PointerGetDatum(hashes[0]);
	values[Anum_pg_dolt_root_roothandles - 1] = PointerGetDatum(hashes[1]);
	values[Anum_pg_dolt_root_rootkeys - 1] = PointerGetDatum(hashes[2]);
	nulls[Anum_pg_dolt_root_roothandles - 1] = !roots->table;
	nulls[Anum_pg_dolt_root_rootkeys - 1] = !roots->table;
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	if (ItemPointerIsValid(tid))
		CatalogTupleUpdate(catalog, tid, tuple);
	else
		CatalogTupleInsert(catalog, tuple);
	heap_freetuple(tuple);
}

/* Publish every written relation, in a fixed order to avoid deadlocks. */
static void
publish_all(void)
{
	HASH_SEQ_STATUS status;
	Overlay    *overlay;
	Overlay   **written;
	int			count = 0;
	Relation	catalog;

	if (!overlays)
		return;
	written = palloc(Max(hash_get_num_entries(overlays), 1) * sizeof(Overlay *));
	hash_seq_init(&status, overlays);
	while ((overlay = hash_seq_search(&status)) != NULL)
		if (overlay->count)
			written[count++] = overlay;
	if (!count)
		return;
	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot publish prolly roots in a read-only transaction")));
	qsort(written, count, sizeof(Overlay *), compare_overlays);
	catalog = table_open(DoltRootRelationId, RowExclusiveLock);
	for (int i = 0; i < count; i++)
	{
		OverlayEntry *top = &written[i]->entries[written[i]->count - 1];
		ItemPointerData tid;
		Roots		latest = roots_lock(catalog, &written[i]->key, &tid);
		Roots		merged;

		/*
		 * Concurrent first publishers of a (storage, branch) take turns: the
		 * later one waits here until the earlier commits, then finds and
		 * locks its row.
		 */
		if (!ItemPointerIsValid(&tid))
		{
			LockDatabaseObject(DoltRootRelationId, written[i]->key.file,
							   (int32) hash_bytes((const unsigned char *) written[i]->key.branch,
												  strlen(written[i]->key.branch)),
							   ExclusiveLock);
			latest = roots_lock(catalog, &written[i]->key, &tid);
		}
		merged = roots_merge(&top->base, &top->roots, &latest);

		if (!merged.present || roots_equal(&merged, &latest))
			continue;
		persist_roots(&merged);
		roots_write(catalog, &written[i]->key, &tid, &merged);
	}
	table_close(catalog, RowExclusiveLock);
	pfree(written);
}

static void
store_xact(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			{
				/*
				 * Commands such as DETACH PARTITION CONCURRENTLY commit while
				 * their portal runs; publish under the transaction's owner.
				 */
				ResourceOwner owner = CurrentResourceOwner;

				CurrentResourceOwner = TopTransactionResourceOwner;
				PG_TRY();
				{
					publish_all();
					working_close(true);
				}
				PG_FINALLY();
				{
					CurrentResourceOwner = owner;
				}
				PG_END_TRY();
			}
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:

			/*
			 * Later pre-commit work, and parallel workers, which publish
			 * nothing, may still have read chunks; close cleanly unless
			 * aborting, when resource owners clean up instead.
			 */
			working_close(event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT);
			overlays = NULL;	/* in TopTransactionContext */
			modified = NULL;
			live_states = NULL;
			live_count = live_capacity = 0;
			collect_threshold = DG_COLLECT_MINIMUM;
			if (working_store)
			{
				pl_store_free(working_store);
				working_store = NULL;
			}
			break;
		default:
			break;
	}
}

static void
store_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS status;
	Overlay    *overlay;

	if (event != SUBXACT_EVENT_ABORT_SUB || !overlays)
		return;

	/* Entries from this subtransaction and its children have ids >= it. */
	hash_seq_init(&status, overlays);
	while ((overlay = hash_seq_search(&status)) != NULL)
		while (overlay->count && overlay->entries[overlay->count - 1].subxid >= mySubid)
			overlay->count--;
	if (modified)
	{
		Modified   *row;

		hash_seq_init(&status, modified);
		while ((row = hash_seq_search(&status)) != NULL)
			if (row->subxid >= mySubid)
				hash_search(modified, &row->key, HASH_REMOVE, NULL);
	}
}

static ModifiedKey
modified_key(Relation rel, const char *branch, uint64 handle)
{
	ModifiedKey key;

	memset(&key, 0, sizeof(key));
	key.root = root_key(rel, branch);
	key.handle = handle;
	return key;
}

/* Note that the current command updated or deleted row |handle|. */
void
dg_note_modified(Relation rel, const char *branch, uint64 handle, CommandId cid)
{
	ModifiedKey key = modified_key(rel, branch, handle);
	Modified   *row;

	if (!modified)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(ModifiedKey);
		ctl.entrysize = sizeof(Modified);
		ctl.hcxt = TopTransactionContext;
		modified = hash_create("prolly modified rows", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	row = hash_search(modified, &key, HASH_ENTER, NULL);
	row->cid = cid;
	row->subxid = GetCurrentSubTransactionId();
}

/* If this transaction updated or deleted row |handle|, the command that did. */
bool
dg_row_modified(Relation rel, const char *branch, uint64 handle, CommandId *cid)
{
	ModifiedKey key;
	Modified   *row;

	if (!modified)
		return false;
	key = modified_key(rel, branch, handle);
	row = hash_search(modified, &key, HASH_FIND, NULL);
	if (row)
		*cid = row->cid;
	return row != NULL;
}

static void
register_callbacks(void)
{
	if (callbacks_registered)
		return;
	RegisterXactCallback(store_xact, NULL);
	RegisterSubXactCallback(store_subxact, NULL);
	callbacks_registered = true;
}

/* ---------------------------------------------------------------------------
 * Handles
 * ---------------------------------------------------------------------------
 */

#define DG_HANDLE_SLOTS 65536

typedef struct HandleSlot
{
	Oid			db;
	Oid			space;
	RelFileNumber file;
	pg_atomic_uint32 used;
	pg_atomic_uint64 next;
} HandleSlot;

typedef struct HandleTable
{
	slock_t		mutex;
	HandleSlot	slots[DG_HANDLE_SLOTS];
} HandleTable;

static HandleTable *handle_table;

static void
handle_table_init(void *ptr)
{
	HandleTable *table = ptr;

	SpinLockInit(&table->mutex);
	for (int i = 0; i < DG_HANDLE_SLOTS; i++)
	{
		pg_atomic_init_u32(&table->slots[i].used, 0);
		pg_atomic_init_u64(&table->slots[i].next, 0);
	}
}

static uint32
handle_slot_start(Oid db, Oid space, RelFileNumber file)
{
	uint64		h = ((uint64) db * UINT64CONST(0x9E3779B97F4A7C15)) ^
		((uint64) space * UINT64CONST(0xC2B2AE3D27D4EB4F)) ^ ((uint64) file * UINT64CONST(0x165667B19E3779F9));

	return (uint32) ((h ^ (h >> 31)) % DG_HANDLE_SLOTS);
}

static HandleSlot *
handle_slot_find(Oid db, Oid space, RelFileNumber file)
{
	uint32		start = handle_slot_start(db, space, file);

	for (uint32 n = 0; n < DG_HANDLE_SLOTS; n++)
	{
		HandleSlot *slot = &handle_table->slots[(start + n) % DG_HANDLE_SLOTS];

		if (!pg_atomic_read_u32(&slot->used))
			return NULL;
		pg_read_barrier();
		if (slot->db == db && slot->space == space && slot->file == file)
			return slot;
	}
	return NULL;
}

/* The largest handle in any non-aborted version of any branch's root row. */
static uint64
max_handle(Oid space, RelFileNumber file)
{
	Relation	catalog = table_open(DoltRootRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	uint64		max = 0;

	ScanKeyInit(&keys[0], Anum_pg_dolt_root_rootspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_root_rootfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(file));
	scan = systable_beginscan(catalog, DoltRootKeyIndexId, true, SnapshotAny, 2, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		TransactionId xmin = HeapTupleHeaderGetXmin(tuple->t_data);
		Roots		roots;
		PLNode	   *node;

		if (!TransactionIdIsCurrentTransactionId(xmin) && !TransactionIdIsInProgress(xmin) &&
			!TransactionIdDidCommit(xmin))
			continue;
		roots = roots_from_tuple(tuple, RelationGetDescr(catalog));
		if (!roots.table)
			continue;

		/* Handles are keys in order: the largest is the rightmost leaf's last. */
		node = pl_find(dg_working_store(), roots.handles);
		while (node && node->level)
		{
			PLHash		child;

			memcpy(child.bytes, pl_node_value(node, node->count - 1).data, PL_HASH_SIZE);
			node = pl_find(dg_working_store(), child);
		}
		if (!node)
			elog(ERROR, "prolly handle tree is unavailable");
		if (node->count)
			max = Max(max, pl_read64(pl_node_key(node, node->count - 1).data));
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return max;
}

uint64
dg_allocate_handle(Relation rel)
{
	Oid			db = rel->rd_locator.dbOid,
				space = rel->rd_locator.spcOid;
	RelFileNumber file = rel->rd_locator.relNumber;
	HandleSlot *slot;
	bool		found;

	if (!handle_table)
		handle_table = GetNamedDSMSegment("dolt prolly handles", sizeof(HandleTable), handle_table_init, &found);
	slot = handle_slot_find(db, space, file);
	if (!slot)
	{
		uint64		max = max_handle(space, file);
		uint32		start = handle_slot_start(db, space, file);

		SpinLockAcquire(&handle_table->mutex);
		for (uint32 n = 0; n < DG_HANDLE_SLOTS; n++)
		{
			HandleSlot *candidate = &handle_table->slots[(start + n) % DG_HANDLE_SLOTS];

			if (pg_atomic_read_u32(&candidate->used))
			{
				if (candidate->db == db && candidate->space == space && candidate->file == file)
				{
					slot = candidate;
					break;
				}
				continue;
			}
			candidate->db = db;
			candidate->space = space;
			candidate->file = file;
			pg_atomic_write_u64(&candidate->next, Max(max, DG_DERIVED_HANDLES) + 1);
			pg_write_barrier();
			pg_atomic_write_u32(&candidate->used, 1);
			slot = candidate;
			break;
		}
		SpinLockRelease(&handle_table->mutex);
		if (!slot)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("too many prolly relations for the handle table")));
	}
	return pg_atomic_fetch_add_u64(&slot->next, 1);
}

void
dg_handle_key(PLHash digest, uint64 handle, uint8 *key)
{
	memcpy(key, digest.bytes, PL_HASH_SIZE);
	pl_write64(key + PL_HASH_SIZE, handle);
}

/* ---------------------------------------------------------------------------
 * Loading and saving states
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * Collecting the working store
 * ---------------------------------------------------------------------------
 */

static void
state_register(DGState *s)
{
	if (live_count == live_capacity)
	{
		live_capacity = live_capacity ? live_capacity * 2 : 16;
		live_states = live_states ? repalloc(live_states, live_capacity * sizeof(DGState *)) :
			MemoryContextAlloc(TopTransactionContext, live_capacity * sizeof(DGState *));
	}
	live_states[live_count++] = s;
}

static void
state_unregister(DGState *s)
{
	for (int i = live_count - 1; i >= 0; i--)
		if (live_states[i] == s)
		{
			live_states[i] = live_states[--live_count];
			return;
		}
}

static void
add_roots(PLHash **roots, int *count, int *capacity, const PLHash *hashes, int n)
{
	if (*count + n > *capacity)
	{
		*capacity = Max(*capacity * 2, *count + n + 64);
		*roots = *roots ? repalloc(*roots, *capacity * sizeof(PLHash)) : palloc(*capacity * sizeof(PLHash));
	}
	memcpy(*roots + *count, hashes, n * sizeof(PLHash));
	*count += n;
}

/*
 * Free working-store nodes nothing live can reach, if the store has grown past
 * the threshold. Callers are table AM write callbacks after releasing their
 * own state, when every tree in use belongs to a registered state or the
 * overlay. Version-control calls and native rewrites hold other roots, so
 * collection waits for them.
 */
void
dg_collect(void)
{
	PLHash	   *roots = NULL;
	int			count = 0,
				capacity = 0;
	HASH_SEQ_STATUS status;
	Overlay    *overlay;

	if (!working_store || pl_store_stats(working_store).bytes < collect_threshold ||
		dg_version_call_active() || dg_native_rewrite_active())
		return;
	for (int i = 0; i < live_count; i++)
	{
		DGState    *s = live_states[i];
		PLHash		trees[3] = {s->content.root, s->handles.root, s->keys.root};

		add_roots(&roots, &count, &capacity, trees, 3);
		if (s->based)
			add_roots(&roots, &count, &capacity, s->base, 3);
	}
	if (overlays)
	{
		hash_seq_init(&status, overlays);
		while ((overlay = hash_seq_search(&status)) != NULL)
			for (int i = 0; i < overlay->count; i++)
			{
				const Roots *both[2] = {&overlay->entries[i].roots, &overlay->entries[i].base};

				for (int j = 0; j < 2; j++)
				{
					PLHash		trees[3] = {both[j]->content, both[j]->handles, both[j]->keys};

					if (both[j]->present)
						add_roots(&roots, &count, &capacity, trees, both[j]->table ? 3 : 1);
				}
			}
	}
	pl_store_retain(working_store, roots, count);
	collect_threshold = Max(DG_COLLECT_MINIMUM, 2 * pl_store_stats(working_store).bytes);
	if (roots)
		pfree(roots);
}

static void
cleanup(void *arg)
{
	DGState    *s = arg;

	if (s->shared && s->store)
		state_unregister(s);
	else if (s->store)
		pl_store_free(s->store);
	s->store = NULL;
}
void
dg_release(DGState *s)
{
	cleanup(s);
}

static DGState *
new_state(Relation rel)
{
	DGState    *s = palloc0(sizeof(*s));

	register_callbacks();
	s->store = dg_working_store();
	s->shared = true;
	state_register(s);
	s->cleanup.func = cleanup;
	s->cleanup.arg = s;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &s->cleanup);
	return s;
}

static void
state_set(DGState *s, const Roots *roots, bool table)
{
	if (roots->present)
	{
		s->content = tree_at(roots->content);
		if (roots->table)
		{
			s->handles = tree_at(roots->handles);
			s->keys = tree_at(roots->keys);
		}
		else
			s->handles = s->keys = empty_tree();
	}
	else
		s->content = s->handles = s->keys = empty_tree();
	s->has_root = roots->present;
}

static bool
tree_has(PLTree *tree, uint64 id)
{
	uint8		key[8];
	PLSlice		k = {key, 8};
	PLValue		value;

	pl_write64(key, id);
	dg_check(pl_get(tree, k, &value));
	return value.present;
}

static DGState *
load_state(Relation rel, Snapshot snap, const uint64 *wanted, const char *branch, bool use_base, bool bound)
{
	DGState    *s;
	RootKey		key;
	Roots		committed,
				view,
				base;
	Overlay    *overlay;
	OverlayEntry *entry;
	bool		table = rel->rd_rel->relkind != RELKIND_INDEX;

	if (!bound)
		dg_catalog_check_relation(rel, branch, snap);
	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	if (snap && !IsMVCCSnapshot(snap) && snap != SnapshotSelf && !wanted)
		dg_unsupported("non-MVCC snapshots");
	s = new_state(rel);
	key = root_key(rel, branch);
	overlay = overlay_find(&key, false);
	if (snap && IsMVCCSnapshot(snap))
		committed = roots_read(&key, snap);
	else
		committed = roots_latest(&key);
	view = base = committed;
	entry = overlay_entry(overlay, snap);
	if (wanted)
	{
		/* SnapshotAny row fetches: the newest own or committed state holding it. */
		entry = NULL;
		for (int i = overlay ? overlay->count - 1 : -1; i >= 0 && !entry; i--)
		{
			PLTree		handles = tree_at(overlay->entries[i].roots.handles);

			if (overlay->entries[i].roots.present && overlay->entries[i].roots.table && tree_has(&handles, *wanted))
				entry = &overlay->entries[i];
		}

		/*
		 * A version another transaction replaced after the statement's
		 * snapshot is gone from the latest roots, but the executor may still
		 * fetch it by TID, as heap fetches dead tuples with SnapshotAny.
		 */
		if (!entry && committed.present && committed.table && ActiveSnapshotSet())
		{
			PLTree		handles = tree_at(committed.handles);

			if (!tree_has(&handles, *wanted))
			{
				Roots		seen = roots_read(&key, GetActiveSnapshot());

				if (seen.present)
					view = base = committed = seen;
			}
		}
	}
	if (entry)
	{
		if (roots_equal(&entry->base, &committed))
		{
			view = entry->roots;
			base = entry->base;
		}
		else
			view = roots_merge(&entry->base, &entry->roots, &committed);
	}
	state_set(s, &view, table);
	s->based = base.present;
	if (base.present)
	{
		s->base[0] = base.content;
		s->base[1] = base.table ? base.handles : base.content;
		s->base[2] = base.table ? base.keys : base.content;
	}
	if (!view.present && use_base)
		dg_branch_base(rel, s);
	return s;
}
/*
 * A branch head's relation without a root of its own on the branch holds the
 * committed content of its working schema member, as bound storage does.
 */
static DGState *
durable_base(Relation rel, DGState *state)
{
	bytea	   *content;

	if (state->has_root)
		return state;
	content = dg_catalog_durable_content(RelationGetRelid(rel));
	if (content)
	{
		state->content = dg_history_import(state->store, content);
		if (rel->rd_rel->relkind == RELKIND_RELATION)
			dg_history_handles(state);
	}
	return state;
}

DGState *
dg_load(Relation rel, Snapshot snap)
{
	if (dg_storage_bound(rel))
		return dg_storage_am_load(rel, snap);
	if (CatalogViewIsDurable(RelationGetRelid(rel)))
		return durable_base(rel, load_state(rel, snap, NULL, dg_relation_branch(rel), false, true));
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		return dg_storage_am_load(rel, snap);

	return load_state(rel, snap, NULL, dg_relation_branch(rel), true, false);
}
DGState *
dg_load_branch(Relation rel, Snapshot snap, const char *branch)
{
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("canonical catalog handles require an explicit storage binding")));

	return load_state(rel, snap, NULL, branch, false, false);
}
DGState *
dg_load_tid(Relation rel, uint64 id)
{
	if (dg_storage_bound(rel))
		return dg_storage_am_load_tid(rel, id);
	if (CatalogViewIsDurable(RelationGetRelid(rel)))
		return durable_base(rel, load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), false, true));
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		return dg_storage_am_load_tid(rel, id);

	return load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), true, false);
}

/* Internal page-reader entrypoint; only dg_resource.c supplies this descriptor. */
DGState *
dg_load_bound(Relation rel, Snapshot snap, const char *branch)
{
	Assert(RelFileNumberIsValid(rel->rd_locator.relNumber));
	Assert(rel->rd_lockInfo.lockRelId.relId < FirstCatalogViewObjectId);
	return load_state(rel, snap, NULL, branch, false, true);
}

DGState *
dg_load_bound_tid(Relation rel, uint64 id, const char *branch)
{
	return load_state(rel, SnapshotSelf, &id, branch, false, true);
}

static void
save_state(Relation rel, DGState *s, CommandId cid, const char *branch)
{
	RootKey		key = root_key(rel, branch);
	Roots		base = {0},
				roots = {.present = true};

	register_callbacks();
	if (s->store != dg_working_store())
		elog(ERROR, "prolly state does not belong to this transaction");
	roots.table = rel->rd_rel->relkind != RELKIND_INDEX;
	roots.content = s->content.root;
	if (roots.table)
	{
		roots.handles = s->handles.root;
		roots.keys = s->keys.root;
	}
	if (s->based)
	{
		base.present = true;
		base.table = roots.table;
		base.content = s->base[0];
		base.handles = s->base[1];
		base.keys = s->base[2];
	}
	overlay_push(&key, cid, &base, &roots);
}
void
dg_save(Relation rel, DGState *s, CommandId cid)
{
	if (dg_storage_bound(rel) || CatalogViewIsLeased(RelationGetRelid(rel)))
		dg_storage_require_write(rel);
	save_state(rel, s, cid, dg_relation_branch(rel));
}

/* Only the transactional resource allocator calls this, before publishing any
 * branch binding: the new storage starts empty on its branch. */
void
dg_initialize_resource(Relation rel, const char *branch)
{
	DGState    *state;

	Assert(CatalogViewObjectContext(RelationGetRelid(rel)));
	state = dg_load_bound(rel, SnapshotSelf, branch);
	save_state(rel, state, GetCurrentCommandId(true), branch);
	dg_release(state);
}

static PLStatus
collect(void *arg, PLSlice key, PLSlice value)
{
	DGRows	   *r = arg;

	if (r->count == r->capacity)
	{
		r->capacity = r->capacity ? r->capacity * 2 : 64;
		r->items = r->items ? repalloc(r->items, r->capacity * sizeof(PLEntry)) : palloc(r->capacity * sizeof(PLEntry));
	}
	r->items[r->count].key = key;
	r->items[r->count++].value = value;
	return PL_OK;
}

DGRows
dg_rows(const PLTree * tree)
{
	DGRows		rows = {0};

	dg_check(pl_scan(tree, NULL, NULL, collect, &rows));
	return rows;
}
void
dg_tid(uint64 id, ItemPointer tid)
{
	if (id / 65535 >= InvalidBlockNumber)
		elog(ERROR, "prolly TID space exhausted");
	ItemPointerSet(tid, id / 65535, (id % 65535) + 1);
}
uint64
dg_id(ItemPointer tid)
{
	return (uint64) ItemPointerGetBlockNumber(tid) * 65535 + ItemPointerGetOffsetNumber(tid) - 1;
}
Datum
dolt_prolly_root(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	DGState    *s;
	char		result[65];
	int			i;

	if (rel->rd_tableam != dg_table_routine() &&
		!(rel->rd_rel->relkind == RELKIND_INDEX && !strcmp(get_am_name(rel->rd_rel->relam), "dolt_prolly_index")))
		dg_unsupported("root inspection of other AMs");
	s = dg_load(rel, GetActiveSnapshot());
	for (i = 0; i < 32; i++)
		sprintf(result + i * 2, "%02x", s->content.root.bytes[i]);
	dg_release(s);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}
