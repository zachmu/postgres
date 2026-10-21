#include "dg.h"
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
	scan->base.rs_rd = rel;
	scan->base.rs_snapshot = snap;
	scan->base.rs_flags = flags;
	scan->context = AllocSetContextCreate(CurrentMemoryContext, "prolly scan", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(scan->context);
	scan->state = dg_load(rel, snap);
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
	dg_decode(base->rs_rd, row_for_handle(scan->state, entry->value), &tid, slot);
	return true;
}
static bool
fetch(Relation rel, ItemPointer tid, Snapshot snap, TupleTableSlot *slot)
{
	DGState    *s = snap == SnapshotAny ? dg_load_tid(rel, dg_id(tid)) : dg_load(rel, snap);
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
	dg_unsupported("following updated row-version chains");
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
static void
writer(Relation rel)
{
	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	LockRelationOid(RelationGetRelid(rel), ExclusiveLock);
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
static void
insert_row(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate)
{
	DGState    *s;
	PLSlice		row;
	PLHash		hash;
	PLEdit		edit;
	uint8		key[8];

	writer(rel);
	s = dg_load(rel, SnapshotSelf);
	row = dg_encode(rel, slot);
	multiplicity(s, row, 1);
	pl_write64(key, s->nextid);
	edit.key.data = key;
	edit.key.len = 8;
	edit.value.present = true;
	hash = dg_rowhash(row);
	edit.value.bytes.data = hash.bytes;
	edit.value.bytes.len = PL_HASH_SIZE;
	dg_check(pl_apply(&s->handles, &edit, 1, &s->handles));
	dg_tid(s->nextid++, &slot->tts_tid);
	dg_save(rel, s, cid);
	dg_release(s);
	pfree((void *) row.data);
	slot->tts_tableOid = RelationGetRelid(rel);
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
static TM_Result
delete_row(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, bool changing)
{
	DGState    *s;
	uint8		key[8];
	PLEdit		edit;
	PLValue		old;
	PLStatus	status;

	writer(rel);
	s = dg_load(rel, SnapshotSelf);
	pl_write64(key, dg_id(tid));
	edit.key.data = key;
	edit.key.len = 8;
	status = pl_get(&s->handles, edit.key, &old);
	if (status == PL_OK && !old.present)
	{
		dg_release(s);
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("prolly row changed; retry transaction")));
	}
	dg_check(status);
	multiplicity(s, row_for_handle(s, old.bytes), -1);
	edit.value.present = false;
	edit.value.bytes.data = NULL;
	edit.value.bytes.len = 0;
	dg_check(pl_apply(&s->handles, &edit, 1, &s->handles));
	dg_save(rel, s, cid);
	dg_release(s);
	dg_rebuild_indexes(rel);
	return TM_Ok;
}
static TM_Result
update_row(Relation rel, ItemPointer tid, TupleTableSlot *slot, CommandId cid, Snapshot snap, Snapshot cross, bool wait, TM_FailureData *failure, LockTupleMode *mode, TU_UpdateIndexes *indexes)
{
	delete_row(rel, tid, cid, snap, cross, wait, failure, false);
	insert_row(rel, slot, cid, 0, NULL);
	*mode = LockTupleExclusive;
	*indexes = TU_All;
	return TM_Ok;
}
static TM_Result
lock_row(Relation r, ItemPointer t, Snapshot s, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy policy, uint8 flags, TM_FailureData *f)
{
	dg_unsupported("row locking");
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
	*pages = RelationGetNumberOfBlocks(rel);
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
	/*
	 * These operations do not alter the stored value layout. pg_dump emits
	 * primary/unique constraints as ALTER TABLE after restoring the data.
	 */
	if (cmd->subtype == AT_AddIndex || cmd->subtype == AT_ChangeOwner)
		return;
	if (cmd->subtype == AT_AddConstraint && IsA(cmd->def, Constraint))
	{
		Constraint *constraint = (Constraint *) cmd->def;

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

/* Install a complete canonical state with fresh runtime occurrence handles.
 * Caller owns an AccessExclusiveLock; never reuse prior executor-visible TIDs. */
void
dg_history_handles(DGState *state)
{
	DGRows		rows = dg_rows(&state->content);
	size_t		i;

	dg_check(pl_build(state->store, NULL, 0, &state->handles));
	for (i = 0; i < rows.count; i++)
	{
		uint64		count = pl_read64(rows.items[i].value.data),
					j;

		CHECK_FOR_INTERRUPTS();
		for (j = 0; j < count; j++)
		{
			uint8		key[8];
			PLEdit		edit;
			ItemPointerData tid;

			CHECK_FOR_INTERRUPTS();
			dg_tid(state->nextid, &tid);
			pl_write64(key, state->nextid++);
			edit.key = (PLSlice)
			{
				key, 8
			};
			edit.value.present = true;
			edit.value.bytes = rows.items[i].key;
			dg_check(pl_apply(&state->handles, &edit, 1, &state->handles));
		}
	}
}
void
dg_history_install(Relation rel, DGState *state, PLTree contents)
{
	state->content = contents;
	dg_history_handles(state);
	dg_save(rel, state, GetCurrentCommandId(true));
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	dg_rebuild_indexes(rel);
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
}
