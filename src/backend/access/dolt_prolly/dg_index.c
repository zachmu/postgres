#include "dg.h"
#include "access/doltbranch.h"
#include "miscadmin.h"
#include "access/amapi.h"
#include "access/nbtree.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_namespace_d.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "common/hashfn.h"
#include "utils/rel.h"

typedef struct DGIndexEntry
{
	PLEntry		entry;
	size_t		prefix;
	bool		nullkey;
}			DGIndexEntry;
typedef struct DGIndexScan
{
	MemoryContext context;
	ItemPointerData *tids;
	HeapTuple  *tuples;
	size_t		count;
	int64		position,
				mark;
}			DGIndexScan;
static int
compare_entries(const void *a, const void *b)
{
	return pl_compare(((const DGIndexEntry *) a)->entry.key, ((const DGIndexEntry *) b)->entry.key);
}
static void
escaped(StringInfo out, PLSlice bytes, bool descending)
{
	size_t		i;
	uint8		xor = descending ? 255 : 0;

	for (i = 0; i < bytes.len; i++)
	{
		appendStringInfoCharMacro(out, bytes.data[i] ^ xor);
		if (!bytes.data[i])
			appendStringInfoCharMacro(out, 255 ^ xor);
	}
	appendStringInfoCharMacro(out, 0 ^ xor);
	appendStringInfoCharMacro(out, 0 ^ xor);
}
/*
 * The index entry key for table row |row| with digest |digest|: each key
 * column as a null tag and escaped canonical bytes, then the escaped digest.
 * *prefix receives the length of the key-column part, and *nullkey whether
 * any key column is null.
 */
static void
index_entry(Relation index, PLSlice row, PLSlice digest, StringInfo key, size_t *prefix, bool *nullkey)
{
	*nullkey = false;
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		int			attribute = index->rd_index->indkey.values[j] - 1;
		PLValue		field;
		bool		desc = (index->rd_indoption[j] & INDOPTION_DESC) != 0;
		bool		nullfirst = (index->rd_indoption[j] & INDOPTION_NULLS_FIRST) != 0;

		if (attribute < 0)
			dg_unsupported("system-column indexes");
		dg_check(pl_tuple_get(row, attribute, &field));
		appendStringInfoChar(key, field.present ? 1 : (nullfirst ? 0 : 2));
		if (field.present)
			escaped(key, dg_key_field(TupleDescAttr(RelationGetDescr(index), j)->atttypid, field.bytes), desc);
		else
			*nullkey = true;
	}
	*prefix = key->len;
	escaped(key, digest, false);
}

/* The btree comparison function the key encoding of |type| orders as. */
static const char *
index_comparator(Oid type)
{
	switch (type)
	{
		case BOOLOID:
			return "btboolcmp";
		case INT2OID:
			return "btint2cmp";
		case INT4OID:
			return "btint4cmp";
		case INT8OID:
			return "btint8cmp";
		case TEXTOID:
		case VARCHAROID:
			return "bttextcmp";
		case BYTEAOID:
			return "byteacmp";
		case BPCHAROID:
			return "bpcharcmp";
		case CHAROID:
			return "btcharcmp";
		case NAMEOID:
			return "btnamecmp";
		case FLOAT4OID:
			return "btfloat4cmp";
		case FLOAT8OID:
			return "btfloat8cmp";
		case NUMERICOID:
			return "numeric_cmp";
		case DATEOID:
			return "date_cmp";
		case TIMEOID:
			return "time_cmp";
		case TIMETZOID:
			return "timetz_cmp";
		case TIMESTAMPOID:
			return "timestamp_cmp";
		case TIMESTAMPTZOID:
			return "timestamptz_cmp";
		case INTERVALOID:
			return "interval_cmp";
		case UUIDOID:
			return "uuid_cmp";
		case CASHOID:
			return "cash_cmp";
		case MACADDROID:
			return "macaddr_cmp";
		case MACADDR8OID:
			return "macaddr8_cmp";
		default:
			return "enum_cmp";
	}
}

/* Reject index definitions the encoding cannot order correctly. */
void
dg_index_validate(Relation table, Relation index)
{
	if (table->rd_tableam != dg_table_routine())
		dg_unsupported("prolly indexes on another table AM");
	if (RelationGetIndexExpressions(index) != NIL || RelationGetIndexPredicate(index) != NIL)
		dg_unsupported("expression and partial indexes");
	if (index->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
		dg_unsupported("temporary or unlogged indexes");
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		int			att = index->rd_index->indkey.values[j];
		Oid			type,
					function = index->rd_support[j];
		const char *expected;
		static const char *operators[] = {"<", "<=", "=", ">=", ">"};
		int			strategy;

		if (att <= 0)
			dg_unsupported("system-column indexes");
		type = getBaseType(TupleDescAttr(RelationGetDescr(table), att - 1)->atttypid);
		if (!dg_key_ordered(type))
			dg_unsupported("ordered index keys of this type");
		if ((type == TEXTOID || type == VARCHAROID || type == BPCHAROID || type == NAMEOID) &&
			!pg_newlocale_from_collation(index->rd_indcollation[j])->collate_is_c)
			dg_unsupported("non-C index collations");
		expected = index_comparator(type);
		if (get_func_namespace(function) != PG_CATALOG_NAMESPACE || strcmp(get_func_name(function), expected))
			dg_unsupported("custom index comparators");

		/* Operator classes are declared on varchar's text and an enum's anyenum. */
		if (type == VARCHAROID)
			type = TEXTOID;
		else if (get_typtype(type) == TYPTYPE_ENUM)
			type = ANYENUMOID;
		for (strategy = 1; strategy <= 5; strategy++)
		{
			Oid			actual = get_opfamily_member(index->rd_opfamily[j], type, type, strategy);
			Oid			wanted = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString(pstrdup(operators[strategy - 1]))), type, type);

			if (!OidIsValid(actual) || actual != wanted)
				dg_unsupported("custom index operators");
		}
	}
	dg_validate_descriptor(RelationGetDescr(table));
}

void
dg_index_rebuild(Relation table, Relation index)
{
	DGState    *source,
			   *target;
	DGRows		rows;
	DGIndexEntry *sorted;
	PLEntry    *entries;
	size_t		i;

	dg_index_validate(table, index);
	source = dg_load(table, SnapshotSelf);
	target = dg_load(index, SnapshotSelf);
	rows = dg_rows(&source->content);
	sorted = palloc0(Max(rows.count, 1) * sizeof(*sorted));
	entries = palloc0(Max(rows.count, 1) * sizeof(*entries));
	for (i = 0; i < rows.count; i++)
	{
		StringInfoData key;
		uint64		count = pl_read64(rows.items[i].value.data);
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		uint8	   *value;

		initStringInfo(&key);
		index_entry(index, row, rows.items[i].key, &key, &sorted[i].prefix, &sorted[i].nullkey);
		sorted[i].entry.key.data = (uint8 *) key.data;
		sorted[i].entry.key.len = key.len;
		value = palloc(8 + row.len);
		pl_write64(value, count);
		memcpy(value + 8, row.data, row.len);
		sorted[i].entry.value.data = value;
		sorted[i].entry.value.len = 8 + row.len;
	}
	qsort(sorted, rows.count, sizeof(*sorted), compare_entries);
	for (i = 0; i < rows.count; i++)
	{
		if (index->rd_index->indisunique && (!sorted[i].nullkey || index->rd_index->indnullsnotdistinct))
		{
			bool		duplicate = pl_read64(sorted[i].entry.value.data) > 1;

			if (i && sorted[i].prefix == sorted[i - 1].prefix && memcmp(sorted[i].entry.key.data, sorted[i - 1].entry.key.data, sorted[i].prefix) == 0)
				duplicate = true;
			if (duplicate)
				ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("duplicate key violates unique constraint \"%s\"", RelationGetRelationName(index))));
		}
		entries[i] = sorted[i].entry;
	}
	dg_check(pl_build(target->store, entries, rows.count, &target->content));
	dg_save(index, target, GetCurrentCommandId(true));
	dg_release(source);
	dg_release(target);
}

/* The prolly indexes of |table| that writes maintain, opened. */
static List *
maintained_indexes(Relation table, bool *opened)
{
	List	   *result = NIL;
	List	   *indexes;
	ListCell   *cell;

	*opened = !dg_storage_bound(table);
	if (dg_storage_bound(table))
		return list_copy(dg_storage_indexes(table));
	indexes = RelationGetIndexList(table);
	foreach(cell, indexes)
	{
		Relation	index = index_open(lfirst_oid(cell), RowExclusiveLock);

		if (strcmp(get_am_name(index->rd_rel->relam), "dolt_prolly_index") != 0)
			dg_unsupported("other index families on prolly tables");
		if (index->rd_index->indisready)
			result = lappend(result, index);
		else
			index_close(index, RowExclusiveLock);
	}
	list_free(indexes);
	return result;
}

static void
close_indexes(List *indexes, bool opened)
{
	ListCell   *cell;

	if (opened)
		foreach(cell, indexes)
			index_close((Relation) lfirst(cell), RowExclusiveLock);
	list_free(indexes);
}

void
dg_rebuild_indexes(Relation rel)
{
	bool		opened;
	List	   *indexes = maintained_indexes(rel, &opened);
	ListCell   *cell;

	foreach(cell, indexes)
		dg_index_rebuild(rel, (Relation) lfirst(cell));
	close_indexes(indexes, opened);
}

static void check_key(Relation table, Relation index, uint64 self, StringInfo key, PLSlice row);
static pg_noreturn void unique_violation(Relation table, Relation index, Datum *values, bool *isnull);

/*
 * Add |delta| occurrences of table row |row|, the version with |handle|, to
 * every maintained index of |table|. Each index entry carries an occurrence
 * count, like the content tree, so this is one path-local edit per index.
 */
void
dg_index_apply(Relation table, PLSlice row, int64 delta, uint64 handle)
{
	bool		opened;
	List	   *indexes = maintained_indexes(table, &opened);
	PLHash		digest = dg_rowhash(row);
	PLSlice		digest_bytes = {digest.bytes, PL_HASH_SIZE};
	ListCell   *cell;

	foreach(cell, indexes)
	{
		Relation	index = lfirst(cell);
		DGState    *state = dg_load(index, SnapshotSelf);
		StringInfoData key;
		size_t		prefix;
		bool		nullkey;
		PLSlice		k;
		PLValue		old;
		int64		count;
		PLEdit		edit = {0};
		uint8	   *value;

		initStringInfo(&key);
		index_entry(index, row, digest_bytes, &key, &prefix, &nullkey);
		k.data = (uint8 *) key.data;
		k.len = key.len;
		dg_check(pl_get(&state->content, k, &old));
		count = (old.present ? (int64) pl_read64(old.bytes.data) : 0) + delta;
		if (count < 0)
			elog(ERROR, "prolly index \"%s\" is missing an entry", RelationGetRelationName(index));
		value = palloc(8 + row.len);
		pl_write64(value, (uint64) count);
		memcpy(value + 8, row.data, row.len);
		edit.key = k;
		edit.value.present = count > 0;
		edit.value.bytes.data = value;
		edit.value.bytes.len = 8 + row.len;
		dg_check(pl_apply(&state->content, &edit, 1, &state->content));
		dg_save(index, state, GetCurrentCommandId(true));
		dg_release(state);
		/*
		 * Writers of bound canonical storage may bypass the executor's index
		 * insertion, so the AM checks their unique indexes itself.
		 */
		if (delta > 0 && dg_storage_bound(table) && index->rd_index->indisunique &&
			(!nullkey || index->rd_index->indnullsnotdistinct))
		{
			key.len = prefix;
			check_key(table, index, handle, &key, row);
		}
		pfree(value);
		pfree(key.data);
	}
	close_indexes(indexes, opened);
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

/* The smallest key greater than every key that starts with |prefix|. */
static bool
prefix_successor(StringInfo prefix)
{
	while (prefix->len > 0 && (uint8) prefix->data[prefix->len - 1] == 0xFF)
		prefix->len--;
	if (prefix->len == 0)
		return false;
	prefix->data[prefix->len - 1]++;
	return true;
}

typedef struct HandleList
{
	uint64	   *items;
	int			count;
	int			capacity;
} HandleList;

static PLStatus
collect_handles(void *arg, PLSlice key, PLSlice value)
{
	HandleList *list = arg;

	if (list->count == list->capacity)
	{
		list->capacity = list->capacity ? list->capacity * 2 : 8;
		list->items = list->items ? repalloc_array(list->items, uint64, list->capacity) :
			palloc_array(uint64, list->capacity);
	}
	list->items[list->count++] = pl_read64(key.data + PL_HASH_SIZE);
	return PL_OK;
}

/* The handles of every occurrence of row |digest| in |table|'s key tree. */
static HandleList
handles_of(DGState *table, PLHash digest)
{
	HandleList	list = {0};
	uint8		lower[DG_KEY_SIZE],
				upper[DG_KEY_SIZE];
	PLSlice		lo = {lower, DG_KEY_SIZE},
				hi = {upper, DG_KEY_SIZE};

	dg_handle_key(digest, 0, lower);
	dg_handle_key(digest, PG_UINT64_MAX, upper);
	dg_check(pl_scan(&table->keys, &lo, &hi, collect_handles, &list));
	return list;
}

typedef struct KeyRows
{
	PLHash	   *digests;
	int			count;
	int			capacity;
} KeyRows;

static PLStatus
collect_key_rows(void *arg, PLSlice key, PLSlice value)
{
	KeyRows    *rows = arg;

	if (rows->count == rows->capacity)
	{
		rows->capacity = rows->capacity ? rows->capacity * 2 : 4;
		rows->digests = rows->digests ? repalloc_array(rows->digests, PLHash, rows->capacity) :
			palloc_array(PLHash, rows->capacity);
	}
	rows->digests[rows->count++] = dg_rowhash((PLSlice) {value.data + 8, value.len - 8});
	return PL_OK;
}

/*
 * Another live row of |table| whose |index| key equals that of the row at
 * |tid|, as this transaction sees the latest state; zero if there is none.
 */
static uint64
unique_conflict(Relation table, Relation index, uint64 self, StringInfo prefix)
{
	DGState    *rows = dg_load(table, SnapshotSelf);
	DGState    *entries = dg_load(index, SnapshotSelf);
	StringInfoData upper;
	PLSlice		lo = {(uint8 *) prefix->data, prefix->len},
				hi;
	KeyRows		found = {0};
	uint64		conflict = 0;

	initStringInfo(&upper);
	appendBinaryStringInfo(&upper, prefix->data, prefix->len);
	if (!prefix_successor(&upper))
		elog(ERROR, "invalid prolly index key prefix");
	hi.data = (uint8 *) upper.data;
	hi.len = upper.len;
	dg_check(pl_scan(&entries->content, &lo, &hi, collect_key_rows, &found));
	for (int i = 0; i < found.count && !conflict; i++)
	{
		HandleList	handles = handles_of(rows, found.digests[i]);

		for (int j = 0; j < handles.count && !conflict; j++)
			if (handles.items[j] != self)
				conflict = handles.items[j];
	}
	dg_release(rows);
	dg_release(entries);
	return conflict;
}

/*
 * Raise a unique violation if another live row has the same key as the row
 * at |tid|, which the table AM already added to |index|.
 *
 * Concurrent inserters of one key serialize on a reservation: a lock record
 * keyed by the index storage and a hash of the key, held until commit. A
 * conflicting row that a running transaction is deleting, or updating with a
 * key change, holds its row lock exclusively; waiting for a key-share lock on
 * it lets that transaction finish first, as heap unique checks wait for the
 * deleting transaction.
 */
void
dg_index_check_unique(Relation table, Relation index, ItemPointer tid, Datum *values, bool *isnull)
{
	DGState    *rows = dg_load(table, SnapshotSelf);
	uint64		self = dg_id(tid);
	uint8		id[8];
	PLSlice		k = {id, 8};
	PLValue		digest;
	PLValue		row;
	StringInfoData key;
	size_t		prefix;
	bool		nullkey;

	pl_write64(id, self);
	dg_check(pl_get(&rows->handles, k, &digest));
	if (!digest.present)
		elog(ERROR, "prolly row to check is missing");
	dg_check(pl_get(&rows->content, digest.bytes, &row));
	if (!row.present)
		elog(ERROR, "missing canonical row contents");
	initStringInfo(&key);
	index_entry(index, (PLSlice) {row.bytes.data + 8, row.bytes.len - 8}, digest.bytes, &key, &prefix, &nullkey);
	dg_release(rows);
	if (nullkey && !index->rd_index->indnullsnotdistinct)
		return;
	key.len = prefix;
	check_key(table, index, self, &key, (PLSlice) {NULL, 0});
}

/*
 * Raise a unique violation if another live row shares |key| (an index key
 * prefix) with row |self|. |row|, if given, describes the key for the error.
 */
static void
check_key(Relation table, Relation index, uint64 self, StringInfo key, PLSlice row)
{
	const char *branch = dg_relation_branch(table);
	uint64		conflict;
	TM_FailureData failure;
	int64		next;

	dg_record_lock(index, dg_relation_branch(index),
				   (int64) hash_bytes_extended((const unsigned char *) key->data, key->len, 0),
				   LockTupleExclusive, LockWaitBlock, false, &failure, &next);
	while ((conflict = unique_conflict(table, index, self, key)) != 0)
	{
		CommandId	cid;
		TupleTableSlot *slot;
		ItemPointerData tid;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		DGState    *rows;
		PLValue		digest,
					found;
		uint8		id[8];
		PLSlice		k = {id, 8};

		if (!dg_row_modified(table, branch, conflict, &cid))
		{
			dg_record_lock(table, branch, (int64) conflict, LockTupleKeyShare, LockWaitBlock, false, &failure, &next);
			if (unique_conflict(table, index, self, key) != conflict)
				continue;
		}

		/* Describe the key from our row, which the error reports. */
		rows = dg_load(table, SnapshotSelf);
		pl_write64(id, self);
		dg_check(pl_get(&rows->handles, k, &digest));
		dg_check(pl_get(&rows->content, digest.bytes, &found));
		slot = table_slot_create(table, NULL);
		dg_tid(self, &tid);
		if (row.data)
			dg_decode(table, row, &tid, slot);
		else
			dg_decode(table, (PLSlice) {found.bytes.data + 8, found.bytes.len - 8}, &tid, slot);
		for (int j = 0; j < index->rd_index->indnatts; j++)
			values[j] = slot_getattr(slot, index->rd_index->indkey.values[j], &isnull[j]);
		unique_violation(table, index, values, isnull);
	}
}

static void
unique_violation(Relation table, Relation index, Datum *values, bool *isnull)
{
	char	   *key = BuildIndexValueDescription(index, values, isnull);

	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 errmsg("duplicate key value violates unique constraint \"%s\"", RelationGetRelationName(index)),
			 key ? errdetail("Key %s already exists.", key) : 0,
			 errtableconstraint(table, RelationGetRelationName(index))));
}
static IndexBuildResult *
build(Relation table, Relation index, IndexInfo *info)
{
	IndexBuildResult *result = palloc0(sizeof(*result));
	DGState    *s;

	if (info->ii_Concurrent)
		dg_unsupported("concurrent index builds");
	dg_index_rebuild(table, index);
	s = dg_load(table, SnapshotSelf);
	result->heap_tuples = result->index_tuples = pl_count(&s->handles);
	dg_release(s);
	return result;
}
static void
build_empty(Relation rel)
{
	dg_unsupported("unlogged indexes");
}
static bool
insert(Relation index, Datum *values, bool *nulls, ItemPointer tid, Relation table, IndexUniqueCheck unique, bool unchanged, IndexInfo *info)
{
	if (unique == UNIQUE_CHECK_PARTIAL || unique == UNIQUE_CHECK_EXISTING)
		dg_unsupported("deferred unique constraints");

	/* The table AM added the entry, and checks bound storage itself. */
	if (unique == UNIQUE_CHECK_YES && !dg_storage_bound(table))
		dg_index_check_unique(table, index, tid, values, nulls);
	return true;
}
static IndexBulkDeleteResult *
bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback cb, void *arg)
{
	dg_unsupported("index vacuum");
}
static IndexBulkDeleteResult *
vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats ? stats : palloc0(sizeof(*stats));
}
static void
cost(PlannerInfo *root, IndexPath *path, double loops, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loops, &costs);
	*startup = costs.indexStartupCost;
	*total = costs.indexTotalCost;
	*selectivity = costs.indexSelectivity;
	*correlation = 0;
	*pages = costs.numIndexPages;
}
static bytea *
options(Datum reloptions, bool validate)
{
	if (DatumGetPointer(reloptions))
		dg_unsupported("index storage options");
	return NULL;
}
static bool
validate(Oid opclass)
{
	return true;
}
static IndexScanDesc
beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);
	DGIndexScan *s = palloc0(sizeof(*s));

	if (norderbys)
		dg_unsupported("ORDER BY operators");
	s->context = AllocSetContextCreate(CurrentMemoryContext, "prolly index scan", ALLOCSET_DEFAULT_SIZES);
	s->position = -1;
	scan->opaque = s;
	return scan;
}
static bool
matches(IndexScanDesc scan, TupleTableSlot *slot)
{
	int			i;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		int			att = scan->indexRelation->rd_index->indkey.values[key->sk_attno - 1];
		bool		null;
		Datum		value = slot_getattr(slot, att, &null);

		if (key->sk_flags & SK_SEARCHNULL)
		{
			if (!null)
				return false;
		}
		else if (key->sk_flags & SK_SEARCHNOTNULL)
		{
			if (null)
				return false;
		}
		else
		{
			if (key->sk_flags & ~SK_ISNULL)
				dg_unsupported("this index scan-key flag");
			if (null || key->sk_flags & SK_ISNULL || !DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, value, key->sk_argument)))
				return false;
		}
	}
	return true;
}
/* Append column |j|'s tag and escaped canonical bytes for scan key |key|. */
static void
append_key_value(IndexScanDesc scan, int j, ScanKey key, StringInfo out)
{
	Relation	index = scan->indexRelation;
	Oid			type = OidIsValid(key->sk_subtype) && key->sk_subtype != ANYENUMOID ? key->sk_subtype :
		TupleDescAttr(RelationGetDescr(scan->heapRelation), index->rd_index->indkey.values[j] - 1)->atttypid;

	appendStringInfoChar(out, 1);
	escaped(out, dg_key_value(type, key->sk_argument), (index->rd_indoption[j] & INDOPTION_DESC) != 0);
}

/*
 * Conservative key bounds for the scan: equality keys on leading columns,
 * then at most one range column. matches() rechecks every candidate, so the
 * bounds only need to contain every match.
 */
static void
scan_bounds(IndexScanDesc scan, StringInfo lower, StringInfo upper, bool *has_lower, bool *has_upper)
{
	Relation	index = scan->indexRelation;
	StringInfoData prefix;

	initStringInfo(&prefix);
	*has_lower = *has_upper = false;
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		ScanKey		equal = NULL,
					low = NULL,
					high = NULL;
		bool		desc = (index->rd_indoption[j] & INDOPTION_DESC) != 0;

		for (int i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey		key = &scan->keyData[i];

			if (key->sk_attno != j + 1 || key->sk_flags != 0)
				continue;
			if (key->sk_strategy == BTEqualStrategyNumber)
				equal = key;
			else if (key->sk_strategy == BTGreaterStrategyNumber || key->sk_strategy == BTGreaterEqualStrategyNumber)
				low = key;
			else if (key->sk_strategy == BTLessStrategyNumber || key->sk_strategy == BTLessEqualStrategyNumber)
				high = key;
		}
		if (equal)
		{
			append_key_value(scan, j, equal, &prefix);
			continue;
		}
		if (desc)
		{
			ScanKey		swap = low;

			low = high;
			high = swap;
		}
		resetStringInfo(lower);
		appendBinaryStringInfo(lower, prefix.data, prefix.len);
		if (low)
			append_key_value(scan, j, low, lower);
		*has_lower = lower->len > 0;
		resetStringInfo(upper);
		appendBinaryStringInfo(upper, prefix.data, prefix.len);
		if (high)
			append_key_value(scan, j, high, upper);
		*has_upper = prefix_successor(upper);
		return;
	}
	resetStringInfo(lower);
	appendBinaryStringInfo(lower, prefix.data, prefix.len);
	*has_lower = lower->len > 0;
	resetStringInfo(upper);
	appendBinaryStringInfo(upper, prefix.data, prefix.len);
	*has_upper = prefix_successor(upper);
}

static void
rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	DGIndexScan *s = scan->opaque;
	MemoryContext old;
	DGState    *index,
			   *table;
	DGRows		rows = {0};
	StringInfoData lower,
				upper;
	bool		has_lower,
				has_upper;
	PLSlice		lo,
				hi;
	size_t		capacity = 16;
	TupleTableSlot *slot;

	if (keys && nkeys)
		memcpy(scan->keyData, keys, sizeof(ScanKeyData) * nkeys);
	MemoryContextReset(s->context);
	old = MemoryContextSwitchTo(s->context);
	index = dg_load(scan->indexRelation, scan->xs_snapshot);
	table = dg_load(scan->heapRelation, scan->xs_snapshot);
	initStringInfo(&lower);
	initStringInfo(&upper);
	scan_bounds(scan, &lower, &upper, &has_lower, &has_upper);
	lo.data = (uint8 *) lower.data;
	lo.len = lower.len;
	hi.data = (uint8 *) upper.data;
	hi.len = upper.len;
	dg_check(pl_scan(&index->content, has_lower ? &lo : NULL, has_upper ? &hi : NULL, collect, &rows));
	s->tids = palloc(capacity * sizeof(ItemPointerData));
	s->tuples = palloc0(capacity * sizeof(HeapTuple));
	s->count = 0;
	s->position = -1;
	s->mark = -1;
	slot = table_slot_create(scan->heapRelation, NULL);
	for (size_t i = 0; i < rows.count; i++)
	{
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		HandleList	handles = handles_of(table, dg_rowhash(row));

		for (int j = 0; j < handles.count; j++)
		{
			ItemPointerData tid;

			CHECK_FOR_INTERRUPTS();
			dg_tid(handles.items[j], &tid);
			dg_decode(scan->heapRelation, row, &tid, slot);
			if (!matches(scan, slot))
				continue;
			if (s->count == capacity)
			{
				capacity *= 2;
				s->tids = repalloc(s->tids, capacity * sizeof(ItemPointerData));
				s->tuples = repalloc0(s->tuples, s->count * sizeof(HeapTuple), capacity * sizeof(HeapTuple));
			}
			if (scan->xs_want_itup)
			{
				Datum		values[INDEX_MAX_KEYS];
				bool		nulls[INDEX_MAX_KEYS];

				for (int k = 0; k < scan->indexRelation->rd_index->indnatts; k++)
					values[k] = slot_getattr(slot, scan->indexRelation->rd_index->indkey.values[k], &nulls[k]);
				s->tuples[s->count] = heap_form_tuple(RelationGetDescr(scan->indexRelation), values, nulls);
			}
			s->tids[s->count++] = tid;
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	dg_release(index);
	dg_release(table);
	MemoryContextSwitchTo(old);
}
static bool
gettuple(IndexScanDesc scan, ScanDirection dir)
{
	DGIndexScan *s = scan->opaque;

	if (ScanDirectionIsBackward(dir))
	{
		if (s->position < 0)
			s->position = s->count;
		--s->position;
	}
	else
		++s->position;
	if (s->position < 0 || s->position >= (int64) s->count)
		return false;
	scan->xs_heaptid = s->tids[s->position];
	scan->xs_recheck = false;
	scan->xs_hitup = s->tuples[s->position];
	scan->xs_hitupdesc = RelationGetDescr(scan->indexRelation);
	return true;
}
static void
endscan(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	MemoryContextDelete(s->context);
	pfree(s);
}
static void
markpos(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	s->mark = s->position;
}
static void
restorepos(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	s->position = s->mark;
}
static bool
canreturn(Relation index, int attribute)
{
	return true;
}
Datum
dolt_prolly_index_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 5;
	am->amsupport = 1;
	am->amcanorder = true;
	am->amcanbackward = true;
	am->amcanunique = true;
	am->amcanmulticol = true;
	am->amoptionalkey = true;
	am->amsearchnulls = true;
	am->amcaninclude = true;
	am->amconsistentequality = true;
	am->amconsistentordering = true;
	am->ambuild = build;
	am->ambuildempty = build_empty;
	am->aminsert = insert;
	am->ambulkdelete = bulkdelete;
	am->amvacuumcleanup = vacuumcleanup;
	am->amcostestimate = cost;
	am->amcanreturn = canreturn;
	am->amoptions = options;
	am->amvalidate = validate;
	am->ambeginscan = beginscan;
	am->amrescan = rescan;
	am->amgettuple = gettuple;
	am->amendscan = endscan;
	am->ammarkpos = markpos;
	am->amrestrpos = restorepos;
	am->amtranslatestrategy = bttranslatestrategy;
	am->amtranslatecmptype = bttranslatecmptype;
	PG_RETURN_POINTER(am);
}
