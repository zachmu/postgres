#include "dg.h"
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

static TableAmRoutine methods;
static bool initialized;

typedef struct DGScan
{
	TableScanDescData base;
	MemoryContext context;
	DGState    *state;
	DGRows		rows;
	int64		position;
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
	scan->rows = dg_rows(&scan->state->handles);
	scan->position = -1;
	MemoryContextSwitchTo(old);
	return &scan->base;
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
	((DGScan *) base)->position = -1;
}
static bool
next(TableScanDesc base, ScanDirection dir, TupleTableSlot *slot)
{
	DGScan	   *scan = (DGScan *) base;
	PLEntry    *entry;
	ItemPointerData tid;

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
		dg_release(s);
		ExecClearTuple(slot);
		return false;
	}
	dg_check(status);
	dg_decode(rel, row_for_handle(s, value.bytes), tid, slot);
	dg_release(s);
	return true;
}
static IndexFetchTableData *
fetch_begin(Relation rel)
{
	IndexFetchTableData *f = palloc0(sizeof(*f));

	f->rel = rel;
	return f;
}
static void
fetch_reset(IndexFetchTableData *f)
{
}
static void
fetch_end(IndexFetchTableData *f)
{
	pfree(f);
}
static bool
fetch_index(IndexFetchTableData *f, ItemPointer tid, Snapshot snap, TupleTableSlot *slot, bool *again, bool *dead)
{
	*again = false;
	if (dead)
		*dead = false;
	return fetch(f->rel, tid, snap, slot);
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
static void
writer(Relation rel)
{
	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	if (rel->rd_catalogview)
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
static void
insert_row(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate)
{
	DGState    *s;
	PLSlice		row;
	uint64		handle;
	bool		rewriting = CatalogViewIsProvisional(rel->rd_catalogview);

	if (rewriting)
		s = dg_catalog_rewrite_state(rel);
	else
	{
		writer(rel);
		s = dg_load(rel, SnapshotSelf);
	}
	row = dg_encode(rel, slot);
	multiplicity(s, row, 1);
	handle = next_handle(rel, s);
	handle_edit(s, handle, dg_rowhash(row), true);
	dg_tid(handle, &slot->tts_tid);
	if (!rewriting)
	{
		dg_save(rel, s, cid);
		dg_release(s);
	}
	if (!rewriting)
		dg_index_apply(rel, row, 1, handle);
	pfree((void *) row.data);
	slot->tts_tableOid = RelationGetRelid(rel);
	if (!rewriting)
		dg_collect();
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
 * Lock row version |handle| in |mode| and decide whether the caller may act on
 * it, as heap_lock_tuple and HeapTupleSatisfiesUpdate do for heap tuples.
 *
 * A row this transaction already updated or deleted is TM_SelfModified, with
 * the command that did it. Otherwise the row's lock record is locked, waiting
 * per |policy|. The version is current if the latest committed state, merged
 * with this transaction's changes, still has it. If not, a transaction that
 * committed after the caller's snapshot replaced it: TM_Updated with the
 * successor in tmfd->ctid if the lock record names one, else TM_Deleted.
 */
static TM_Result
lock_version(Relation rel, uint64 handle, Snapshot snapshot, LockTupleMode mode,
			 LockWaitPolicy policy, TM_FailureData *tmfd)
{
	const char *branch = dg_relation_branch(rel);
	CommandId	cid;
	int64		heir = 0;
	DGState    *s;
	PLHash		digest;
	bool		present;

	dg_tid(handle, &tmfd->ctid);
	tmfd->xmax = InvalidTransactionId;
	tmfd->cmax = InvalidCommandId;
	tmfd->traversed = false;
	if (dg_row_modified(rel, branch, handle, &cid))
	{
		tmfd->xmax = GetTopTransactionIdIfAny();
		tmfd->cmax = cid;
		if (snapshot && IsMVCCSnapshot(snapshot) && cid < snapshot->curcid)
			return TM_Invisible;
		return TM_SelfModified;
	}
	{
		TM_Result	result = dg_lock_record(rel, branch, (int64) handle, mode, policy, &heir);

		if (result != TM_Ok)
			return result;
	}
	s = dg_load(rel, SnapshotSelf);
	present = handle_digest(s, handle, &digest);
	dg_release(s);
	if (present)
		return TM_Ok;
	if (heir)
	{
		dg_tid((uint64) heir, &tmfd->ctid);
		return TM_Updated;
	}
	return TM_Deleted;
}

static TM_Result
delete_row(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, bool changing)
{
	DGState    *s;
	uint64		handle = dg_id(tid);
	PLHash		digest;
	PLSlice		row;
	TM_Result	result;

	writer(rel);
	result = lock_version(rel, handle, snapshot, LockTupleExclusive,
						  wait ? LockWaitBlock : LockWaitError, failure);
	if (result != TM_Ok)
		return result;
	s = dg_load(rel, SnapshotSelf);
	if (!handle_digest(s, handle, &digest))
		elog(ERROR, "locked prolly row is missing");
	row = row_for_handle(s, (PLSlice) {digest.bytes, PL_HASH_SIZE});
	row = copy_slice(row);
	multiplicity(s, row, -1);
	handle_edit(s, handle, digest, false);
	dg_save(rel, s, cid);
	dg_release(s);
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
static TM_Result
update_row(Relation rel, ItemPointer tid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, LockTupleMode *mode, TU_UpdateIndexes *indexes)
{
	DGState    *s;
	uint64		handle = dg_id(tid),
				successor;
	PLHash		old_digest,
				new_digest;
	PLSlice		old_row = {0},
				new_row;
	bool		key_change = true,
				indexed_change = true;
	TM_Result	result;

	writer(rel);
	new_row = dg_encode(rel, slot);
	new_digest = dg_rowhash(new_row);
	s = dg_load(rel, SnapshotSelf);
	if (handle_digest(s, handle, &old_digest))
	{
		PLSlice		row = row_for_handle(s, (PLSlice) {old_digest.bytes, PL_HASH_SIZE});
		Bitmapset  *keys = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY);
		Bitmapset  *indexed = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_HOT_BLOCKING);

		old_row = copy_slice(row);
		key_change = columns_differ(old_row, new_row, keys);
		indexed_change = columns_differ(old_row, new_row, indexed);
	}
	dg_release(s);
	*mode = key_change ? LockTupleExclusive : LockTupleNoKeyExclusive;
	successor = dg_allocate_handle(rel);
	result = lock_version(rel, handle, snapshot, *mode, wait ? LockWaitBlock : LockWaitError, failure);
	if (result != TM_Ok)
		return result;
	dg_lock_set_heir(rel, dg_relation_branch(rel), (int64) handle, (int64) successor);

	/* The row may have changed between the first read and the lock. */
	s = dg_load(rel, SnapshotSelf);
	if (!handle_digest(s, handle, &old_digest))
		elog(ERROR, "locked prolly row is missing");
	{
		PLSlice		row = row_for_handle(s, (PLSlice) {old_digest.bytes, PL_HASH_SIZE});

		old_row = copy_slice(row);
	}
	multiplicity(s, old_row, -1);
	multiplicity(s, new_row, 1);
	handle_edit(s, handle, old_digest, false);
	handle_edit(s, successor, new_digest, true);
	dg_save(rel, s, cid);
	dg_release(s);
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
	DGState    *s;
	PLHash		digest;

	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	for (;;)
	{
		result = lock_version(rel, dg_id(tid), snapshot, mode, policy, tmfd);
		if (result != TM_Updated || !(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
			break;

		/* Read Committed: follow the update chain to the latest version. */
		*tid = tmfd->ctid;
		traversed = true;
	}
	tmfd->traversed = traversed;
	if (result != TM_Ok)
		return result;
	s = dg_load(rel, SnapshotSelf);
	if (!handle_digest(s, dg_id(tid), &digest))
		elog(ERROR, "locked prolly row is missing");
	dg_decode(rel, row_for_handle(s, (PLSlice) {digest.bytes, PL_HASH_SIZE}), tid, slot);
	dg_release(s);
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
	dg_validate_descriptor(RelationGetDescr(rel));
	GetHeapamTableAmRoutine()->relation_set_new_filelocator(rel, loc, persistence, xid, multi);
}
static void
cluster(Relation a, Relation b, Relation c, bool sort, TransactionId oldest, TransactionId *xid, MultiXactId *multi, double *rows, double *vacuumed, double *dead)
{
	dg_unsupported("CLUSTER/VACUUM FULL");
}
static void
vacuum_rel(Relation rel, struct VacuumParams *p, BufferAccessStrategy s)
{
	dg_unsupported("VACUUM (version retention/freezing is not implemented)");
}
static bool
analyze_block(TableScanDesc s, ReadStream *stream)
{
	dg_unsupported("ANALYZE");
}
static bool
analyze_tuple(TableScanDesc s, TransactionId oldest, double *live, double *dead, TupleTableSlot *slot)
{
	dg_unsupported("ANALYZE");
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
	*pages = rel->rd_catalogview ? 1 : RelationGetNumberOfBlocks(rel);
	*tuples = Max(1000, *pages);
	*allvis = 0;
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
		 cmd->subtype == AT_SetNotNull || cmd->subtype == AT_DropNotNull || cmd->subtype == AT_AddColumn || cmd->subtype == AT_DropColumn || cmd->subtype == AT_ColumnDefault || cmd->subtype == AT_AddConstraint || cmd->subtype == AT_DropConstraint || cmd->subtype == AT_ValidateConstraint || cmd->subtype == AT_AlterColumnType))
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
		methods.scan_bitmap_next_tuple = NULL;
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
