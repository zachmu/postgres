#include "dg.h"
#include <math.h>
#include "nodes/tidbitmap.h"
#include "optimizer/plancat.h"
#include "access/doltbranch.h"
#include "miscadmin.h"
#include "catalog/catalogview.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "access/sysattr.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"

/* Planner page accounting: rows per 8 kB "page" of a prolly table. */
#define DG_ESTIMATE_BYTES_PER_ROW 32

static TableAmRoutine methods;
static bool initialized;

typedef struct DGScan
{
	TableScanDescData base;
	MemoryContext context;
	DGState    *state;

	/*
	 * A forward scan walks the content tree and the key tree in step: both
	 * are ordered by row digest, so each row's handles follow its bytes
	 * without a lookup. Backward scans materialize the handles instead.
	 */
	bool		streaming;
	PLCursor	content;
	PLCursor	keys;
	DGRows		rows;
	bool		materialized;
	int64		position;
	TBMIterateResult bitmap_page;
	OffsetNumber bitmap_offsets[TBM_MAX_TUPLES_PER_PAGE];
	int			bitmap_count;
	int			bitmap_position;
}			DGScan;

PLHash
dg_rowhash(PLSlice row)
{
	PLHash		hash;

	if (!pl_hash_bytes(row, &hash))
		elog(ERROR, "could not hash canonical row");
	return hash;
}
static PLSlice
row_for_handle(DGState *state, PLSlice hash)
{
	PLValue		value;
	PLSlice		row;

	if (hash.len != PL_HASH_SIZE)
		elog(ERROR, "invalid prolly row reference");
	dg_check(pl_get(&state->content, hash, &value));
	if (!value.present || value.bytes.len < 8)
		elog(ERROR, "missing canonical row contents");
	row.data = value.bytes.data + 8;
	row.len = value.bytes.len - 8;
	return row;
}
static const TupleTableSlotOps *
slots(Relation rel)
{
	return &TTSOpsVirtual;
}
static TableScanDesc
begin(Relation rel, Snapshot snap, int nkeys, ScanKey key, ParallelTableScanDesc parallel, uint32 flags)
{
	DGScan	   *scan = palloc0(sizeof(*scan));
	MemoryContext old;

	if (parallel || nkeys)
		dg_unsupported("parallel scans or table scan keys");
	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	dg_validate_descriptor(RelationGetDescr(rel));
	/* ANALYZE opens its sample scan without a snapshot; read the live state. */
	if (!snap && (flags & SO_TYPE_ANALYZE))
		snap = GetActiveSnapshot();
	scan->base.rs_rd = rel;
	scan->base.rs_snapshot = snap;
	scan->base.rs_flags = flags;
	scan->context = AllocSetContextCreate(CurrentMemoryContext, "prolly scan", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(scan->context);
	scan->state = CatalogViewIsProvisional(rel->rd_catalogview) ? dg_catalog_mutation_load(rel, snap) : dg_load(rel, snap);
	scan->streaming = !(flags & SO_TYPE_BITMAPSCAN) && !CatalogViewIsProvisional(rel->rd_catalogview) && scan->state->has_root;
	if (scan->streaming)
	{
		dg_check(pl_cursor_init(&scan->content, &scan->state->content, NULL));
		dg_check(pl_cursor_init(&scan->keys, &scan->state->keys, NULL));
	}
	else if (!(flags & SO_TYPE_BITMAPSCAN))
	{
		scan->rows = dg_rows(&scan->state->handles);
		scan->materialized = true;
	}
	scan->position = -1;
	MemoryContextSwitchTo(old);
	return &scan->base;
}

/* The handles of a scan, for the directions the merged walk does not serve. */
static void
materialize(DGScan *scan)
{
	MemoryContext old = MemoryContextSwitchTo(scan->context);

	if (!scan->materialized)
	{
		scan->rows = dg_rows(&scan->state->handles);
		scan->materialized = true;
	}
	scan->streaming = false;
	MemoryContextSwitchTo(old);
}
static void
end(TableScanDesc base)
{
	DGScan	   *scan = (DGScan *) base;

	if (base->rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(base->rs_snapshot);
	MemoryContextDelete(scan->context);
	pfree(scan);
}
static void
rescan(TableScanDesc base, ScanKey key, bool set, bool strat, bool sync, bool pagemode)
{
	DGScan	   *scan = (DGScan *) base;

	scan->position = -1;
	scan->bitmap_count = 0;
	scan->bitmap_position = 0;
	if (scan->streaming)
	{
		dg_check(pl_cursor_init(&scan->content, &scan->state->content, NULL));
		dg_check(pl_cursor_init(&scan->keys, &scan->state->keys, NULL));
	}
}

/* The next row of the merged walk: the content entry each key entry names. */
static bool
next_streamed(DGScan *scan, TupleTableSlot *slot)
{
	for (;;)
	{
		PLEntry		content,
					key;
		int			order;
		ItemPointerData tid;
		PLSlice		row;

		if (!scan->keys.valid || !scan->content.valid)
		{
			ExecClearTuple(slot);
			return false;
		}
		key = pl_cursor_entry(&scan->keys);
		content = pl_cursor_entry(&scan->content);
		if (key.key.len != DG_KEY_SIZE || content.key.len != PL_HASH_SIZE || content.value.len < 8)
			elog(ERROR, "invalid prolly scan entry");
		order = memcmp(key.key.data, content.key.data, PL_HASH_SIZE);
		if (order > 0)
		{
			dg_check(pl_cursor_next(&scan->content));
			continue;
		}
		if (order < 0)
		{
			/* A key without its row: stale, as a handle-only tree never is. */
			dg_check(pl_cursor_next(&scan->keys));
			continue;
		}
		dg_tid(pl_read64(key.key.data + PL_HASH_SIZE), &tid);
		row.data = content.value.data + 8;
		row.len = content.value.len - 8;
		dg_check(pl_cursor_next(&scan->keys));
		dg_decode(scan->base.rs_rd, row, &tid, slot);
		return true;
	}
}

static bool
next(TableScanDesc base, ScanDirection dir, TupleTableSlot *slot)
{
	DGScan	   *scan = (DGScan *) base;
	PLEntry    *entry;
	ItemPointerData tid;

	if (scan->streaming && ScanDirectionIsForward(dir))
		return next_streamed(scan, slot);
	if (scan->streaming)
		materialize(scan);
	if (ScanDirectionIsBackward(dir))
	{
		if (scan->position < 0)
			scan->position = scan->rows.count;
		--scan->position;
	}
	else
		++scan->position;
	if (scan->position < 0 || scan->position >= (int64) scan->rows.count)
	{
		ExecClearTuple(slot);
		return false;
	}
	entry = &scan->rows.items[scan->position];
	dg_tid(pl_read64(entry->key.data), &tid);
	if (CatalogViewIsProvisional(base->rs_rd->rd_catalogview))
		dg_decode_mutation(base->rs_rd, row_for_handle(scan->state, entry->value), &tid, slot);
	else
		dg_decode(base->rs_rd, row_for_handle(scan->state, entry->value), &tid, slot);
	return true;
}
/* Bitmap blocks are logical handle ranges, not physical relation pages. */
static bool
bitmap_next(TableScanDesc base, TupleTableSlot *slot, bool *recheck,
			uint64 *lossy_pages, uint64 *exact_pages)
{
	DGScan	   *scan = (DGScan *) base;

	for (;;)
	{
		ItemPointerData tid;
		uint8		key[8];
		PLValue		value;
		PLSlice		row;

		CHECK_FOR_INTERRUPTS();
		if (scan->bitmap_position >= scan->bitmap_count)
		{
			if (!tbm_iterate(&base->st.rs_tbmiterator, &scan->bitmap_page))
			{
				ExecClearTuple(slot);
				return false;
			}
			scan->bitmap_position = 0;
			if (scan->bitmap_page.lossy)
			{
				/* Visit every possible handle on a lossy logical page. */
				scan->bitmap_count = TBM_MAX_TUPLES_PER_PAGE;
				++*lossy_pages;
			}
			else
			{
				scan->bitmap_count = tbm_extract_page_tuple(&scan->bitmap_page,
					scan->bitmap_offsets, TBM_MAX_TUPLES_PER_PAGE);
				++*exact_pages;
			}
		}
		ItemPointerSet(&tid, scan->bitmap_page.blockno,
			scan->bitmap_page.lossy ? scan->bitmap_position + 1 :
			scan->bitmap_offsets[scan->bitmap_position]);
		++scan->bitmap_position;
		*recheck = scan->bitmap_page.lossy || scan->bitmap_page.recheck;
		if (!CatalogViewIsProvisional(base->rs_rd->rd_catalogview) &&
			dg_stash_get(RelationGetRelid(base->rs_rd), scan->state, dg_id(&tid), &row))
		{
			dg_decode(base->rs_rd, row, &tid, slot);
			return true;
		}
		pl_write64(key, dg_id(&tid));
		dg_check(pl_get(&scan->state->handles, (PLSlice) {key, sizeof(key)}, &value));
		if (!value.present)
			continue;
		if (CatalogViewIsProvisional(base->rs_rd->rd_catalogview))
			dg_decode_mutation(base->rs_rd, row_for_handle(scan->state, value.bytes), &tid, slot);
		else
			dg_decode(base->rs_rd, row_for_handle(scan->state, value.bytes), &tid, slot);
		return true;
	}
}
/* Release a state loaded for |rel|; a provisional rewrite keeps its own. */
static void
release_state(Relation rel, DGState *s)
{
	if (CatalogViewIsProvisional(rel->rd_catalogview))
		dg_catalog_mutation_release(s);
	else
		dg_release(s);
}
static bool
fetch(Relation rel, ItemPointer tid, Snapshot snap, TupleTableSlot *slot)
{
	DGState    *s = CatalogViewIsProvisional(rel->rd_catalogview) ?
		dg_catalog_mutation_load(rel, snap) :
		(snap == SnapshotAny ? dg_load_tid(rel, dg_id(tid)) : dg_load(rel, snap));
	uint8		key[8];
	PLSlice		k = {key, 8};
	PLValue		value;
	PLStatus	status;

	pl_write64(key, dg_id(tid));
	status = pl_get(&s->handles, k, &value);
	if (status == PL_OK && !value.present)
	{
		release_state(rel, s);
		ExecClearTuple(slot);
		return false;
	}
	dg_check(status);
	dg_decode(rel, row_for_handle(s, value.bytes), tid, slot);
	release_state(rel, s);
	return true;
}
/* An index scan's fetches share one state, loaded under the scan's snapshot. */
typedef struct DGFetch
{
	IndexFetchTableData base;
	MemoryContext memory;		/* the scan's, where the state lives */
	Snapshot	snapshot;
	DGState    *state;
} DGFetch;

/* ---------------------------------------------------------------------------
 * The row stash
 * ---------------------------------------------------------------------------
 *
 * An index entry's value holds its row, and the index scan finds the row's
 * handles to return them as TIDs. The table fetch for each TID would then
 * descend the handle tree and the content tree again to reach the same
 * bytes. The scan stashes each row under its handle instead, tagged with
 * the content and handle roots it was read under; a fetch under the same
 * roots takes the row from the stash. The stash is bounded, and cleared as
 * a whole when it fills.
 */
typedef struct StashKey
{
	Oid			relid;
	uint64		handle;
} StashKey;

typedef struct StashEntry
{
	StashKey	key;
	PLHash		content,
				handles;
	PLSlice		row;
} StashEntry;

#define DG_STASH_BYTES ((Size) 16 * 1024 * 1024)

static HTAB *stash;
static MemoryContext stash_memory;
static Size stash_bytes;

static void
stash_reset(void)
{
	HASHCTL		ctl = {0};

	if (stash_memory)
		MemoryContextReset(stash_memory);
	else
		stash_memory = AllocSetContextCreate(TopMemoryContext, "prolly row stash", ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(StashKey);
	ctl.entrysize = sizeof(StashEntry);
	ctl.hcxt = stash_memory;
	stash = hash_create("prolly row stash", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	stash_bytes = 0;
}

void
dg_stash_put(Oid relid, const DGState *state, uint64 handle, PLSlice row)
{
	StashKey	key = {relid, handle};
	StashEntry *entry;
	bool		found;

	if (!stash || stash_bytes + row.len > DG_STASH_BYTES)
		stash_reset();
	entry = hash_search(stash, &key, HASH_ENTER, &found);
	if (found)
	{
		if (pl_hash_equal(entry->content, state->content.root) && pl_hash_equal(entry->handles, state->handles.root))
			return;
		stash_bytes -= entry->row.len;
		pfree(entry->row.data);
	}
	entry->content = state->content.root;
	entry->handles = state->handles.root;
	entry->row.data = MemoryContextAlloc(stash_memory, Max(row.len, 1));
	entry->row.len = row.len;
	memcpy(entry->row.data, row.data, row.len);
	stash_bytes += row.len;
}

bool
dg_stash_get(Oid relid, const DGState *state, uint64 handle, PLSlice *row)
{
	StashKey	key = {relid, handle};
	StashEntry *entry;

	if (!stash)
		return false;
	entry = hash_search(stash, &key, HASH_FIND, NULL);
	if (!entry || !pl_hash_equal(entry->content, state->content.root) || !pl_hash_equal(entry->handles, state->handles.root))
		return false;
	*row = entry->row;
	return true;
}

static IndexFetchTableData *
fetch_begin(Relation rel)
{
	DGFetch    *f = palloc0(sizeof(*f));

	f->base.rel = rel;
	f->memory = CurrentMemoryContext;
	return &f->base;
}
static void
fetch_reset(IndexFetchTableData *f)
{
}
static void
fetch_end(IndexFetchTableData *base)
{
	DGFetch    *f = (DGFetch *) base;

	if (f->state)
		dg_release(f->state);
	pfree(f);
}
static bool
fetch_index(IndexFetchTableData *base, ItemPointer tid, Snapshot snap, TupleTableSlot *slot, bool *again, bool *dead)
{
	DGFetch    *f = (DGFetch *) base;
	Relation	rel = base->rel;
	uint8		key[8];
	PLSlice		k = {key, 8},
				row;
	PLValue		value;

	*again = false;
	if (dead)
		*dead = false;
	if (CatalogViewIsProvisional(rel->rd_catalogview) || snap == SnapshotAny)
		return fetch(rel, tid, snap, slot);
	if (f->state && f->snapshot != snap)
	{
		dg_release(f->state);
		f->state = NULL;
	}
	if (!f->state)
	{
		MemoryContext old = MemoryContextSwitchTo(f->memory);

		f->state = dg_load(rel, snap);
		f->snapshot = snap;
		MemoryContextSwitchTo(old);
	}
	if (dg_stash_get(RelationGetRelid(rel), f->state, dg_id(tid), &row))
	{
		dg_decode(rel, row, tid, slot);
		return true;
	}
	pl_write64(key, dg_id(tid));
	dg_check(pl_get(&f->state->handles, k, &value));
	if (!value.present)
	{
		ExecClearTuple(slot);
		return false;
	}
	dg_decode(rel, row_for_handle(f->state, value.bytes), tid, slot);
	return true;
}
static bool
tid_valid(TableScanDesc s, ItemPointer tid)
{
	return ItemPointerIsValid(tid);
}
static void
latest(TableScanDesc s, ItemPointer tid)
{
	/* Successors are found by locking; an unlocked lookup keeps the TID. */
}
static bool
satisfies(Relation rel, TupleTableSlot *slot, Snapshot snap)
{
	return fetch(rel, &slot->tts_tid, snap, slot);
}
static TransactionId
index_delete(Relation rel, TM_IndexDeleteOp *op)
{
	dg_unsupported("physical index tuple deletion");
}
/*
 * Writers take no table-level write lock: they lock the rows they change, and
 * publication merges disjoint changes. Canonical storage bindings still check
 * their write authority.
 */
/* A revision is history: its rows can be neither changed nor locked. */
static void
read_only_revision(Relation rel)
{
	if (CatalogViewIsLeased(RelationGetRelid(rel)) && !dg_storage_bound(rel) &&
		!CatalogViewIsProvisional(rel->rd_catalogview))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("relation \"%s\" is a revision, which is read-only", RelationGetRelationName(rel))));
}

static void
writer(Relation rel)
{
	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	read_only_revision(rel);
	if (dg_storage_bound(rel) || CatalogViewIsLeased(RelationGetRelid(rel)))
		dg_storage_require_write(rel);
}
static void
multiplicity(DGState *s, PLSlice row, int delta)
{
	PLHash		hash = dg_rowhash(row);
	PLSlice		key = {hash.bytes, PL_HASH_SIZE};
	PLValue		old;
	uint64		count;
	uint8	   *bytes;
	PLEdit		edit;

	dg_check(pl_get(&s->content, key, &old));
	count = old.present ? pl_read64(old.bytes.data) : 0;
	if (old.present && (old.bytes.len != row.len + 8 || memcmp(old.bytes.data + 8, row.data, row.len)))
		elog(ERROR, "canonical row hash collision");
	if (delta < 0 && !count)
		elog(ERROR, "missing canonical row");
	count = delta > 0 ? count + 1 : count - 1;
	bytes = palloc(8 + row.len);
	pl_write64(bytes, count);
	memcpy(bytes + 8, row.data, row.len);
	edit.key = key;
	edit.value.present = count > 0;
	edit.value.bytes.data = bytes;
	edit.value.bytes.len = 8 + row.len;
	dg_check(pl_apply(&s->content, &edit, 1, &s->content));
	pfree(bytes);
}
/* Map |handle| to |digest|, or remove that mapping, in the handle and key trees. */
static void
handle_edit(DGState *s, uint64 handle, PLHash digest, bool present)
{
	uint8		id[8];
	uint8		key[DG_KEY_SIZE];
	PLEdit		edit = {0};

	pl_write64(id, handle);
	edit.key.data = id;
	edit.key.len = 8;
	edit.value.present = present;
	edit.value.bytes.data = digest.bytes;
	edit.value.bytes.len = present ? PL_HASH_SIZE : 0;
	dg_check(pl_apply(&s->handles, &edit, 1, &s->handles));
	dg_handle_key(digest, handle, key);
	edit.key.data = key;
	edit.key.len = DG_KEY_SIZE;
	edit.value.bytes.data = present ? key : NULL;
	edit.value.bytes.len = 0;
	dg_check(pl_apply(&s->keys, &edit, 1, &s->keys));
}

static uint64
next_handle(Relation rel, DGState *s)
{
	return s->nextid ? s->nextid++ : dg_allocate_handle(rel);
}
/*
 * An ordinary insert records its edits in the command's batch, applied to
 * the trees in one pass when something else reads them; a rewrite's state
 * is private and takes each edit directly.
 */
static void
insert_row(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate)
{
	PLSlice		row;
	uint64		handle;

	if (CatalogViewIsProvisional(rel->rd_catalogview))
	{
		DGState    *s = dg_catalog_rewrite_state(rel);

		row = dg_encode(rel, slot);
		multiplicity(s, row, 1);
		handle = next_handle(rel, s);
		handle_edit(s, handle, dg_rowhash(row), true);
	}
	else
	{
		DGBatch    *batch;

		writer(rel);
		batch = dg_batch(rel, cid);
		row = dg_encode(rel, slot);
		handle = next_handle(rel, dg_batch_state(batch));
		dg_batch_insert(batch, row, handle);
		dg_index_apply(rel, row, 1, handle);
		dg_collect();
	}
	dg_tid(handle, &slot->tts_tid);
	pfree((void *) row.data);
	slot->tts_tableOid = RelationGetRelid(rel);
}
/* The end of a COPY or multi-row INSERT: apply what the command recorded. */
static void
finish_bulk_insert(Relation rel, int options)
{
	if (!CatalogViewIsProvisional(rel->rd_catalogview))
		dg_batch_finish(rel);
}
static void
multi_insert(Relation rel, TupleTableSlot **slots_, int n, CommandId cid, int opts, BulkInsertState state)
{
	int			i;

	for (i = 0; i < n; i++)
		insert_row(rel, slots_[i], cid, opts, state);
}
static void
speculative(Relation r, TupleTableSlot *s, CommandId c, int o, BulkInsertState b, uint32 token)
{
	dg_unsupported("ON CONFLICT/speculative insertion");
}
static void
complete(Relation r, TupleTableSlot *s, uint32 token, bool succeeded)
{
	dg_unsupported("speculative insertion");
}
static PLSlice
copy_slice(PLSlice bytes)
{
	uint8	   *copy = palloc(Max(bytes.len, 1));

	memcpy(copy, bytes.data, bytes.len);
	return (PLSlice) {copy, bytes.len};
}

/* The digest row |handle| maps to in |s|; false if the handle is absent. */
static bool
handle_digest(DGState *s, uint64 handle, PLHash *digest)
{
	uint8		key[8];
	PLSlice		k = {key, 8};
	PLValue		value;

	pl_write64(key, handle);
	dg_check(pl_get(&s->handles, k, &value));
	if (value.present)
		memcpy(digest->bytes, value.bytes.data, PL_HASH_SIZE);
	return value.present;
}

/*
 * Whether this transaction already updated or deleted row version |handle|,
 * and if so the result heap gives: TM_SelfModified for the current or a later
 * command, TM_Invisible for an earlier one.
 */
static bool
self_modified(Relation rel, uint64 handle, Snapshot snapshot, TM_FailureData *tmfd, TM_Result *result)
{
	CommandId	cid;

	if (!dg_row_modified(rel, dg_relation_branch(rel), handle, &cid))
		return false;
	dg_tid(handle, &tmfd->ctid);
	tmfd->xmax = GetTopTransactionIdIfAny();
	tmfd->cmax = cid;
	tmfd->traversed = false;
	*result = snapshot && IsMVCCSnapshot(snapshot) && cid < snapshot->curcid ? TM_Invisible : TM_SelfModified;
	return true;
}

/*
 * After its lock record allowed a change, whether row version |handle| still
 * exists. A history install (merge, reset) replaces rows without touching lock
 * records, so a version can disappear without its record knowing.
 */
static TM_Result
still_current(Relation rel, uint64 handle, TM_Result result, TM_FailureData *tmfd)
{
	DGBatch    *b;
	DGState    *s;
	PLHash		digest;
	bool		present;

	if (result != TM_Ok)
		return result;
	if ((b = dg_batch_fresh(rel)) != NULL)
		present = dg_batch_handle_digest(b, handle, &digest);
	else
	{
		s = dg_load(rel, SnapshotSelf);
		present = handle_digest(s, handle, &digest);
		dg_release(s);
	}
	if (present)
		return TM_Ok;
	dg_tid(handle, &tmfd->ctid);
	return TM_Deleted;
}

/*
 * Referential actions under Repeatable Read pass the transaction snapshot as a
 * crosscheck: a row it cannot see was added after the snapshot, so changing
 * it is a serialization failure, as in heap_delete.
 */
static bool
crosscheck_visible(Relation rel, Snapshot crosscheck, uint64 handle)
{
	DGState    *s;
	PLHash		digest;
	bool		visible;

	if (crosscheck == InvalidSnapshot)
		return true;
	s = dg_load(rel, crosscheck);
	visible = handle_digest(s, handle, &digest);
	dg_release(s);
	return visible;
}

static TM_Result
delete_row(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, bool changing)
{
	DGBatch    *b;
	uint64		handle = dg_id(tid);
	PLHash		digest;
	PLSlice		row;
	TM_Result	result;
	int64		next;

	writer(rel);
	if (self_modified(rel, handle, snapshot, failure, &result))
		return result;
	result = dg_record_delete(rel, dg_relation_branch(rel), (int64) handle, wait, changing, failure, &next);
	if (result == TM_Ok && !crosscheck_visible(rel, crosscheck, handle))
		result = TM_Updated;
	result = still_current(rel, handle, result, failure);
	if (result != TM_Ok)
		return result;
	b = dg_batch_latest(rel, cid);
	if (!dg_batch_handle_digest(b, handle, &digest))
		elog(ERROR, "locked prolly row is missing");
	if (!dg_batch_row(b, digest, &row))
		elog(ERROR, "missing canonical row");
	row = copy_slice(row);
	dg_batch_remove(b, row, handle);
	dg_index_apply(rel, row, -1, handle);
	dg_note_modified(rel, dg_relation_branch(rel), handle, cid);
	dg_collect();
	return TM_Ok;
}

/* Whether the old and new rows differ in any column of |attributes|. */
static bool
columns_differ(PLSlice old, PLSlice new, Bitmapset *attributes)
{
	int			member = -1;

	while ((member = bms_next_member(attributes, member)) >= 0)
	{
		int			attribute = member + FirstLowInvalidHeapAttributeNumber - 1;
		PLValue		a,
					b;

		if (attribute < 0)
			continue;
		dg_check(pl_tuple_get(old, attribute, &a));
		dg_check(pl_tuple_get(new, attribute, &b));
		if (a.present != b.present ||
			(a.present && (a.bytes.len != b.bytes.len || memcmp(a.bytes.data, b.bytes.data, a.bytes.len))))
			return true;
	}
	return false;
}

/*
 * Replace row version |tid| with the slot's row. The new version gets a new
 * handle, as a heap update makes a new tuple, so AFTER triggers and RETURNING
 * can still fetch the old version by its TID. The old version's lock record
 * names the new one for transactions that waited on it.
 */
/*
 * The row version |handle| as this transaction sees it, with its digest; a
 * version a concurrent update already replaced in the latest state may still
 * be in the caller's |snapshot|. Read through the command's batch while its
 * trees are the latest, so a statement's earlier rows stay pending.
 */
static bool
current_row(Relation rel, uint64 handle, Snapshot snapshot, PLHash *digest, PLSlice *row)
{
	DGBatch    *b = dg_batch_fresh(rel);
	DGState    *s;
	bool		present;

	if (b && dg_batch_handle_digest(b, handle, digest))
	{
		if (!dg_batch_row(b, *digest, row))
			elog(ERROR, "missing canonical row");
		*row = copy_slice(*row);
		return true;
	}
	s = dg_load(rel, SnapshotSelf);
	if (!handle_digest(s, handle, digest) && snapshot && IsMVCCSnapshot(snapshot))
	{
		dg_release(s);
		s = dg_load(rel, snapshot);
	}
	present = handle_digest(s, handle, digest);
	if (present)
		*row = copy_slice(row_for_handle(s, (PLSlice) {digest->bytes, PL_HASH_SIZE}));
	dg_release(s);
	return present;
}

static TM_Result
update_row(Relation rel, ItemPointer tid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, LockTupleMode *mode, TU_UpdateIndexes *indexes)
{
	DGBatch    *b;
	uint64		handle = dg_id(tid),
				successor;
	PLHash		old_digest,
				new_digest;
	PLSlice		old_row = {0},
				new_row;
	bool		key_change = true,
				indexed_change = true;
	TM_Result	result;
	int64		next;

	writer(rel);
	new_row = dg_encode(rel, slot);
	new_digest = dg_rowhash(new_row);
	/*
	 * Judge the key change from the version being updated. A concurrent
	 * update may already have replaced it in the latest state; the caller's
	 * snapshot still has it.
	 */
	if (current_row(rel, handle, snapshot, &old_digest, &old_row))
	{
		Bitmapset  *keys = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY);
		Bitmapset  *indexed = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_HOT_BLOCKING);

		key_change = columns_differ(old_row, new_row, keys);
		indexed_change = columns_differ(old_row, new_row, indexed);
	}
	*mode = key_change ? LockTupleExclusive : LockTupleNoKeyExclusive;
	if (self_modified(rel, handle, snapshot, failure, &result))
		return result;
	successor = dg_allocate_handle(rel);
	result = dg_record_update(rel, dg_relation_branch(rel), (int64) handle, (int64) successor, key_change,
							  wait, failure, &next);
	if (result == TM_Ok && !crosscheck_visible(rel, crosscheck, handle))
		result = TM_Updated;
	result = still_current(rel, handle, result, failure);
	if (result != TM_Ok)
		return result;

	/* The row may have changed between the first read and the lock. */
	b = dg_batch_latest(rel, cid);
	if (!dg_batch_handle_digest(b, handle, &old_digest))
		elog(ERROR, "locked prolly row is missing");
	if (!dg_batch_row(b, old_digest, &old_row))
		elog(ERROR, "missing canonical row");
	old_row = copy_slice(old_row);
	dg_batch_remove(b, old_row, handle);
	dg_batch_insert(b, new_row, successor);
	if (!pl_hash_equal(old_digest, new_digest))
	{
		dg_index_apply(rel, old_row, -1, handle);
		dg_index_apply(rel, new_row, 1, successor);
	}
	dg_note_modified(rel, dg_relation_branch(rel), handle, cid);
	dg_tid(successor, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	*indexes = indexed_change ? TU_All : TU_None;
	dg_collect();
	return TM_Ok;
}

static TM_Result
lock_row(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy policy, uint8 flags, TM_FailureData *tmfd)
{
	TM_Result	result;
	bool		traversed = false;
	int64		next;

	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	read_only_revision(rel);
	for (;;)
	{
		if (self_modified(rel, dg_id(tid), snapshot, tmfd, &result))
			break;
		result = dg_record_lock(rel, dg_relation_branch(rel), (int64) dg_id(tid), mode, policy,
								(flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0, tmfd, &next);
		if (result == TM_Updated && (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
			ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
		if (result != TM_Updated || !(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
			break;

		/* Read Committed: follow the update chain to the latest version. */
		*tid = tmfd->ctid;
		traversed = true;
	}
	tmfd->traversed = traversed;
	if (result != TM_Ok)
		return result;

	/*
	 * A key-share lock may hold a version a committed non-key update replaced,
	 * as heap allows; fetch whichever version the TID names.
	 */
	if (!fetch(rel, tid, SnapshotAny, slot))
	{
		dg_tid(dg_id(tid), &tmfd->ctid);
		return TM_Deleted;
	}
	return TM_Ok;
}
static Size
parallel_estimate(Relation r)
{
	dg_unsupported("parallel scans");
}
static Size
parallel_init(Relation r, ParallelTableScanDesc p)
{
	dg_unsupported("parallel scans");
}
static void
parallel_reset(Relation r, ParallelTableScanDesc p)
{
	dg_unsupported("parallel scans");
}
static void
new_storage(Relation rel, const RelFileLocator *loc, char persistence, TransactionId *xid, MultiXactId *multi)
{
	if (persistence != RELPERSISTENCE_PERMANENT)
		dg_unsupported("temporary/unlogged tables");

	/*
	 * Prolly tables live on branches: CREATE TABLE publishes them to the
	 * branch's schema and allocates their storage there. Physical prolly
	 * relations, from CREATE TABLE AS, SET ACCESS METHOD and the like, are
	 * not supported yet.
	 */
	if (!rel->rd_catalogview)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("prolly tables can only be created with CREATE TABLE")));
	dg_validate_descriptor(RelationGetDescr(rel));
	GetHeapamTableAmRoutine()->relation_set_new_filelocator(rel, loc, persistence, xid, multi);
}
static void
cluster(Relation a, Relation b, Relation c, bool sort, TransactionId oldest, TransactionId *xid, MultiXactId *multi, double *rows, double *vacuumed, double *dead)
{
	dg_unsupported("CLUSTER/VACUUM FULL");
}
/*
 * Prolly rows carry no transaction IDs to freeze, and replaced versions live
 * in the chunk store, whose garbage collection is separate. VACUUM has
 * nothing to do here.
 */
static void
vacuum_rel(Relation rel, struct VacuumParams *p, BufferAccessStrategy s)
{
}

/*
 * The relation file holds no blocks, so ANALYZE samples nothing; planning
 * uses relation_estimate_size instead of sampled statistics.
 */
static bool
analyze_block(TableScanDesc s, ReadStream *stream)
{
	return false;
}
static bool
analyze_tuple(TableScanDesc s, TransactionId oldest, double *live, double *dead, TupleTableSlot *slot)
{
	return false;
}
static double
build_range(Relation t, Relation i, IndexInfo *info, bool sync, bool any, bool progress, BlockNumber start, BlockNumber n, IndexBuildCallback cb, void *arg, TableScanDesc scan)
{
	dg_unsupported("external index build callbacks");
}
static void
validate_index(Relation t, Relation i, IndexInfo *info, Snapshot s, struct ValidateIndexState *v)
{
	dg_unsupported("concurrent index builds");
}
static bool
toast(Relation rel)
{
	return false;
}
static void
estimate(Relation rel, int32 *widths, BlockNumber *pages, double *tuples, double *allvis)
{
	DGState *state = CatalogViewIsProvisional(rel->rd_catalogview) ?
		dg_catalog_mutation_load(rel, GetActiveSnapshot()) :
		dg_load(rel, GetActiveSnapshot());
	double bytes;

	/* Physical adapter pages do not describe a revision's row count. The
	 * handle root carries the exact count, including duplicate occurrences.
	 * Use logical row volume for costing until chunk statistics are exposed.
	 */
	*tuples = pl_count(&state->handles);

	/*
	 * A row costs about the same to reach by handle as in sequence: there are
	 * no pages to seek, and the type-declared width the planner would size
	 * pages by overstates stored rows several times over. Report few pages,
	 * so that plans are chosen on per-row costs rather than on page fetches.
	 */
	bytes = *tuples * DG_ESTIMATE_BYTES_PER_ROW;
	*pages = (BlockNumber) Min((double) MaxBlockNumber,
		Max(1.0, ceil(bytes / BLCKSZ)));

	/*
	 * A prolly table is never analyzed, so nothing corrects an estimate the
	 * planner makes from a small table's exact size. Count a small table as
	 * a thousand rows, as before exact counts, so that plans over small tables
	 * keep using their indexes.
	 */
	*tuples = Max(*tuples, 1000);

	/* Index entries are versioned with the rows: index-only scans fetch nothing. */
	*allvis = 1;
	release_state(rel, state);
}
static bool
sample_block(TableScanDesc s, struct SampleScanState *state)
{
	dg_unsupported("TABLESAMPLE");
}
static bool
sample_tuple(TableScanDesc s, struct SampleScanState *state, TupleTableSlot *slot)
{
	dg_unsupported("TABLESAMPLE");
}
static const char *
index_am(Relation rel, const char *requested)
{
	if (strcmp(requested, "btree") && strcmp(requested, "dolt_prolly_index"))
		dg_unsupported("other index families");
	return "dolt_prolly_index";
}
static void
validate_alter(Relation rel, struct AlterTableCmd *cmd)
{
	if (CatalogViewIsProvisional(rel->rd_catalogview) &&
		(cmd->subtype == AT_AlterConstraint || cmd->subtype == AT_SetStatistics || cmd->subtype == AT_SetOptions ||
		 cmd->subtype == AT_ResetOptions || cmd->subtype == AT_SetStorage || cmd->subtype == AT_SetCompression ||
		 cmd->subtype == AT_SetNotNull || cmd->subtype == AT_DropNotNull || cmd->subtype == AT_AddColumn || cmd->subtype == AT_DropColumn || cmd->subtype == AT_ColumnDefault || cmd->subtype == AT_CookedColumnDefault || cmd->subtype == AT_AddConstraint || cmd->subtype == AT_DropConstraint || cmd->subtype == AT_ValidateConstraint || cmd->subtype == AT_AlterColumnType ||
		 cmd->subtype == AT_AddIdentity || cmd->subtype == AT_SetIdentity || cmd->subtype == AT_DropIdentity))
		return;

	/*
	 * These operations do not alter the stored value layout. pg_dump emits
	 * primary/unique constraints as ALTER TABLE after restoring the data.
	 */
	if (cmd->subtype == AT_AddIndex || cmd->subtype == AT_ChangeOwner)
		return;
	if (cmd->subtype == AT_AddConstraint && IsA(cmd->def, Constraint))
	{
		Constraint *constraint = (Constraint *) cmd->def;

		if (constraint->contype == CONSTR_FOREIGN)
			return;
		if ((constraint->contype == CONSTR_PRIMARY || constraint->contype == CONSTR_UNIQUE) &&
			!constraint->deferrable)
			return;
	}

	/*
	 * Restored primary-key columns are already NOT NULL. Permit only the
	 * idempotent SET NOT NULL generated while rebuilding their constraint.
	 */
	if (cmd->subtype == AT_SetNotNull)
	{
		AttrNumber	attnum = get_attnum(RelationGetRelid(rel), cmd->name);

		if (attnum > 0 && TupleDescAttr(RelationGetDescr(rel), attnum - 1)->attnotnull)
			return;
	}
	dg_unsupported("ALTER TABLE (schema versioning is not implemented)");
}
const TableAmRoutine *
dg_table_routine(void)
{
	if (!initialized)
	{
		methods = *GetHeapamTableAmRoutine();
		methods.relation_index_am = index_am;
		methods.relation_validate_alter = validate_alter;
		methods.relation_begin_rewrite = dg_catalog_begin_rewrite;
		methods.relation_finish_rewrite = dg_catalog_finish_rewrite;
		methods.slot_callbacks = slots;
		methods.scan_begin = begin;
		methods.scan_end = end;
		methods.scan_rescan = rescan;
		methods.scan_getnextslot = next;
		methods.scan_set_tidrange = NULL;
		methods.scan_getnextslot_tidrange = NULL;
		methods.parallelscan_estimate = parallel_estimate;
		methods.parallelscan_initialize = parallel_init;
		methods.parallelscan_reinitialize = parallel_reset;
		methods.index_fetch_begin = fetch_begin;
		methods.index_fetch_reset = fetch_reset;
		methods.index_fetch_end = fetch_end;
		methods.index_fetch_tuple = fetch_index;
		methods.tuple_fetch_row_version = fetch;
		methods.tuple_tid_valid = tid_valid;
		methods.tuple_get_latest_tid = latest;
		methods.tuple_satisfies_snapshot = satisfies;
		methods.index_delete_tuples = index_delete;
		methods.tuple_insert = insert_row;
		methods.multi_insert = multi_insert;
		methods.finish_bulk_insert = finish_bulk_insert;
		methods.index_only_visible = true;
		methods.tuple_insert_speculative = speculative;
		methods.tuple_complete_speculative = complete;
		methods.tuple_delete = delete_row;
		methods.tuple_update = update_row;
		methods.tuple_lock = lock_row;
		methods.relation_set_new_filelocator = new_storage;
		methods.relation_copy_for_cluster = cluster;
		methods.relation_vacuum = vacuum_rel;
		methods.scan_analyze_next_block = analyze_block;
		methods.scan_analyze_next_tuple = analyze_tuple;
		methods.index_build_range_scan = build_range;
		methods.index_validate_scan = validate_index;
		methods.relation_needs_toast_table = toast;
		methods.relation_estimate_size = estimate;
		methods.scan_bitmap_next_tuple = bitmap_next;
		methods.scan_sample_next_block = sample_block;
		methods.scan_sample_next_tuple = sample_tuple;
		initialized = true;
	}
	return &methods;
}
Datum
dolt_prolly_table_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(dg_table_routine());
}

/*
 * Give every row occurrence in |state|'s content a derived handle, numbered
 * 1..n in content order. The same content always derives the same handles, so
 * readers of a branch that has no roots of its own agree on TIDs. Allocated
 * handles start above DG_DERIVED_HANDLES, so the two never collide.
 */
void
dg_history_handles(DGState *state)
{
	DGRows		rows = dg_rows(&state->content);
	PLEntry    *handles = NULL,
			   *keys = NULL;
	uint8	   *ids = NULL,
			   *keybytes = NULL;
	size_t		total = 0,
				n = 0;

	for (size_t i = 0; i < rows.count; i++)
		total += pl_read64(rows.items[i].value.data);
	if (total >= DG_DERIVED_HANDLES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("too many prolly rows to derive handles")));
	if (total)
	{
		handles = palloc_array(PLEntry, total);
		keys = palloc_array(PLEntry, total);
		ids = palloc(total * 8);
		keybytes = palloc(total * DG_KEY_SIZE);
	}
	for (size_t i = 0; i < rows.count; i++)
	{
		uint64		count = pl_read64(rows.items[i].value.data);
		PLHash		digest;

		CHECK_FOR_INTERRUPTS();
		memcpy(digest.bytes, rows.items[i].key.data, PL_HASH_SIZE);
		for (uint64 j = 0; j < count; j++, n++)
		{
			pl_write64(ids + n * 8, n + 1);
			handles[n].key = (PLSlice) {ids + n * 8, 8};
			handles[n].value = rows.items[i].key;
			dg_handle_key(digest, n + 1, keybytes + n * DG_KEY_SIZE);
			keys[n].key = (PLSlice) {keybytes + n * DG_KEY_SIZE, DG_KEY_SIZE};
			keys[n].value = (PLSlice) {keybytes, 0};
		}
	}
	dg_check(pl_build(state->store, handles, total, &state->handles));
	dg_check(pl_build(state->store, keys, total, &state->keys));
}

typedef struct HandleChange
{
	PLHash		digest;
	int64		delta;
} HandleChange;

typedef struct HandleChanges
{
	HandleChange *items;
	int			count;
	int			capacity;
} HandleChanges;

static PLStatus
collect_change(void *arg, PLSlice key, PLValue before, PLValue after)
{
	HandleChanges *changes = arg;
	int64		delta = (after.present ? (int64) pl_read64(after.bytes.data) : 0) -
		(before.present ? (int64) pl_read64(before.bytes.data) : 0);

	if (!delta)
		return PL_OK;
	if (changes->count == changes->capacity)
	{
		changes->capacity = changes->capacity ? changes->capacity * 2 : 64;
		changes->items = changes->items ? repalloc_array(changes->items, HandleChange, changes->capacity) :
			palloc_array(HandleChange, changes->capacity);
	}
	memcpy(changes->items[changes->count].digest.bytes, key.data, PL_HASH_SIZE);
	changes->items[changes->count++].delta = delta;
	return PL_OK;
}

typedef struct FoundHandles
{
	uint64	   *handles;
	int64		wanted;
	int64		count;
} FoundHandles;

static PLStatus
collect_handle(void *arg, PLSlice key, PLSlice value)
{
	FoundHandles *found = arg;

	if (found->count == found->wanted)
		return PL_CONFLICT;		/* stops the scan */
	found->handles[found->count++] = pl_read64(key.data + PL_HASH_SIZE);
	return PL_OK;
}

/*
 * Replace |state|'s content with |contents|, keeping the handles of row
 * occurrences present in both: only rows the diff adds or removes gain or lose
 * handles. Cost is proportional to the difference, not the table.
 */
static void
install_handles(Relation rel, DGState *state, PLTree contents)
{
	PLTree		before = state->content;
	HandleChanges changes = {0};

	dg_check(pl_diff(&before, &contents, collect_change, &changes));
	for (int i = 0; i < changes.count; i++)
	{
		HandleChange *change = &changes.items[i];

		CHECK_FOR_INTERRUPTS();
		if (change->delta > 0)
		{
			for (int64 j = 0; j < change->delta; j++)
				handle_edit(state, next_handle(rel, state), change->digest, true);
		}
		else
		{
			uint8		lower[DG_KEY_SIZE],
						upper[DG_KEY_SIZE];
			PLSlice		lo = {lower, DG_KEY_SIZE},
						hi = {upper, DG_KEY_SIZE};
			FoundHandles found = {.wanted = -change->delta};
			PLStatus	status;

			found.handles = palloc_array(uint64, found.wanted);
			dg_handle_key(change->digest, 0, lower);
			dg_handle_key(change->digest, PG_UINT64_MAX, upper);
			status = pl_scan(&state->keys, &lo, &hi, collect_handle, &found);
			if (status != PL_CONFLICT)
				dg_check(status);
			if (found.count != found.wanted)
				elog(ERROR, "prolly key tree is missing row handles");
			for (int64 j = 0; j < found.count; j++)
				handle_edit(state, found.handles[j], change->digest, false);
			pfree(found.handles);
		}
	}
	state->content = contents;
}

void
dg_history_install(Relation rel, DGState *state, PLTree contents)
{
	/* Bound resources retain their original snapshot during root publication. */
	if (rel->rd_catalogview)
		PushCopiedSnapshot(GetActiveSnapshot());
	install_handles(rel, state, contents);
	dg_save(rel, state, GetCurrentCommandId(true));
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	dg_rebuild_indexes(rel);
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	if (rel->rd_catalogview)
		PopActiveSnapshot();
}

/* TRUNCATE of a branch's table: an empty root for the branch, and its indexes. */
void
dg_truncate(Relation rel)
{
	DGState    *state;
	PLTree		empty;

	writer(rel);
	state = dg_load(rel, SnapshotSelf);
	dg_check(pl_build(state->store, NULL, 0, &empty));
	dg_history_install(rel, state, empty);
	dg_release(state);
}
