#include "dg.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"
#include "fmgr.h"
#include <openssl/evp.h>

PG_FUNCTION_INFO_V1(dolt_prolly_table_handler);
static TableAmRoutine methods;
static bool initialized;

typedef struct DGScan
{
	TableScanDescData base;
	MemoryContext context;
	DGState    *state;
	DGRows		rows;
	int64		position;
} DGScan;

void
dg_validate_descriptor(TupleDesc desc)
{
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(desc, i);

		if (a->attisdropped)
			dg_unsupported("dropped columns");
		switch (a->atttypid)
		{
			case BOOLOID:
			case INT2OID:
			case INT4OID:
			case INT8OID:
			case BYTEAOID:
				break;
			case TEXTOID:
				if (!pg_newlocale_from_collation(a->attcollation)->collate_is_c)
					dg_unsupported("non-C text collations");
				break;
			default:
				dg_unsupported("this column type (supported: boolean, smallint, integer, bigint, text COLLATE C, bytea)");
		}
	}
}
PLSlice
dg_encode(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	PLValue    *fields = palloc0(sizeof(PLValue) * desc->natts);
	uint8		(*fixed)[8] = palloc0(8 * desc->natts);
	uint8	   *packed;
	size_t		length;
	int			i;
	PLSlice		result;

	dg_validate_descriptor(desc);
	slot_getallattrs(slot);
	for (i = 0; i < desc->natts; i++)
	{
		Datum		d = slot->tts_values[i];
		Oid			type = TupleDescAttr(desc, i)->atttypid;

		fields[i].present = !slot->tts_isnull[i];
		if (!fields[i].present)
			continue;
		if (type == TEXTOID || type == BYTEAOID)
		{
			struct varlena *v = PG_DETOAST_DATUM_PACKED(d);

			fields[i].bytes.data = (uint8 *) VARDATA_ANY(v);
			fields[i].bytes.len = VARSIZE_ANY_EXHDR(v);
		}
		else
		{
			int64		n = type == BOOLOID ? DatumGetBool(d) : type == INT2OID ? DatumGetInt16(d) : type == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);

			pl_key_int64(n, fixed[i]);
			fields[i].bytes.data = fixed[i];
			fields[i].bytes.len = 8;
		}
	}
	dg_check(pl_tuple_pack(fields, desc->natts, &packed, &length));
	result.data = palloc(length);
	result.len = length;
	memcpy((void *) result.data, packed, length);
	free(packed);
	pfree(fields);
	pfree(fixed);
	return result;
}
PLHash
dg_rowhash(PLSlice row)
{
	PLHash		hash;
	unsigned int length = 0;

	if (EVP_Digest(row.data, row.len, hash.bytes, &length, EVP_sha256(), NULL) != 1 || length != PL_HASH_SIZE)
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
void
dg_decode(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values = palloc0(sizeof(Datum) * desc->natts);
	bool	   *nulls = palloc0(sizeof(bool) * desc->natts);
	HeapTuple	tuple;
	int			i;

	if (bytes.len < 12 || pl_read32(bytes.data + 8) != (uint32) desc->natts)
		dg_unsupported("schema changes to stored tuples");
	for (i = 0; i < desc->natts; i++)
	{
		PLValue		field;
		Oid			type = TupleDescAttr(desc, i)->atttypid;

		dg_check(pl_tuple_get(bytes, i, &field));
		nulls[i] = !field.present;
		if (!field.present)
			continue;
		if (type == TEXTOID || type == BYTEAOID)
		{
			struct varlena *v = palloc(VARHDRSZ + field.bytes.len);

			SET_VARSIZE(v, VARHDRSZ + field.bytes.len);
			memcpy(VARDATA(v), field.bytes.data, field.bytes.len);
			values[i] = PointerGetDatum(v);
		}
		else
		{
			int64		n;

			if (field.bytes.len != 8)
				elog(ERROR, "invalid prolly integer encoding");
			n = (int64) (pl_read64(field.bytes.data) ^ UINT64CONST(0x8000000000000000));
			values[i] = type == BOOLOID ? BoolGetDatum(n != 0) : type == INT2OID ? Int16GetDatum(n) : type == INT4OID ? Int32GetDatum(n) : Int64GetDatum(n);
		}
	}
	tuple = heap_form_tuple(desc, values, nulls);
	tuple->t_self = *tid;
	tuple->t_tableOid = RelationGetRelid(rel);
	for (i = 0; i < desc->natts; i++)
		if (!nulls[i] && !TupleDescAttr(desc, i)->attbyval)
			pfree(DatumGetPointer(values[i]));
	pfree(values);
	pfree(nulls);
	ExecForceStoreHeapTuple(tuple, slot, true);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
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
	/* Index creation does not change the tuple descriptor. */
	if (cmd->subtype != AT_AddIndex)
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
