#include "dg.h"
#include "catalog/catalogview.h"
#include "access/doltbranch.h"
#include "access/doltcatalog.h"
#include "access/generic_xlog.h"
#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"

#define DG_MAGIC 0x44475034
#define DG_HEADER 56
#define DG_PART (BLCKSZ - 128)

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
static void
cleanup(void *arg)
{
	DGState    *s = arg;

	pl_store_free(s->store);
	s->store = NULL;
}
void
dg_release(DGState *s)
{
	cleanup(s);
}
static bool
visible(TransactionId xid, CommandId cid, Snapshot snap)
{
	if (TransactionIdIsCurrentTransactionId(xid))
		return snap == SnapshotSelf || (IsMVCCSnapshot(snap) && cid < snap->curcid);
	if (!TransactionIdDidCommit(xid))
		return false;
	if (snap == SnapshotSelf)
		return true;
	if (!IsMVCCSnapshot(snap))
		dg_unsupported("non-MVCC snapshots");
	return !XidInMVCCSnapshot(xid, snap);
}
static DGState *
load_state(Relation rel, Snapshot snap, const uint64 *wanted, const char *branch, bool use_base, bool bound)
{
	DGState    *s = palloc0(sizeof(*s));
	bool		found_root = false;
	PLHash		binding;
	PLSlice		branchbytes = {(const uint8 *) branch, strlen(branch)};
	HASHCTL		ctl = {0};
	BlockNumber n,
				b;
	uint8	   *assembly = NULL;
	uint32		assembled = 0,
				total = 0;
	PLHash		assembling = {{0}};

	if (!bound)
		dg_catalog_check_relation(rel, branch, snap);
	n = RelationGetNumberOfBlocks(rel);
	if (IsolationIsSerializable())
		dg_unsupported("SERIALIZABLE isolation");
	if (!pl_hash_bytes(branchbytes, &binding))
		elog(ERROR, "branch hash failed");
	s->store = pl_store_create();
	if (!s->store)
		ereport(ERROR, (errmsg("out of memory creating prolly store")));
	s->cleanup.func = cleanup;
	s->cleanup.arg = s;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &s->cleanup);
	ctl.keysize = sizeof(PLHash);
	ctl.entrysize = sizeof(PLHash);
	ctl.hcxt = CurrentMemoryContext;
	s->persisted = hash_create("persisted prolly chunks", 128, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	dg_check(pl_build(s->store, NULL, 0, &s->content));
	s->handles = s->content;
	s->nextid = 1;
	for (b = 0; b < n; b++)
	{
		Buffer		buf = ReadBuffer(rel, b);
		Page		page;
		uint8	   *data;
		Size		size;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		if (PageGetMaxOffsetNumber(page) != 1)
			elog(ERROR, "invalid prolly page");
		size = ItemIdGetLength(PageGetItemId(page, 1));
		data = palloc(size);
		memcpy(data, PageGetItem(page, PageGetItemId(page, 1)), size);
		UnlockReleaseBuffer(buf);
		if (size < DG_HEADER || pl_read32(data) != DG_MAGIC)
			elog(ERROR, "invalid prolly record");
		if (pl_read32(data + 4) == 1)
		{
			uint32		length = pl_read32(data + 8),
						offset = pl_read32(data + 12);
			PLHash		hash;

			memcpy(hash.bytes, data + 16, 32);
			if (!offset)
			{
				if (assembly)
					pfree(assembly);
				if (length > MaxAllocSize || length < 24)
					elog(ERROR, "invalid prolly chunk length");
				assembly = palloc(length);
				total = length;
				assembled = 0;
				assembling = hash;
			}
			if (!assembly || length != total || offset != assembled || !pl_hash_equal(hash, assembling) || size - DG_HEADER > total - assembled)
				elog(ERROR, "invalid prolly chunk fragments");
			memcpy(assembly + assembled, data + DG_HEADER, size - DG_HEADER);
			assembled += size - DG_HEADER;
			if (assembled == total)
			{
				PLSlice		bytes = {assembly, total};

				dg_check(pl_store_import(s->store, hash, bytes));
				hash_search(s->persisted, &hash, HASH_ENTER, NULL);
				pfree(assembly);
				assembly = NULL;
			}
		}
		else if (pl_read32(data + 4) == 2)
		{
			if (size != DG_HEADER + 112)
				elog(ERROR, "invalid prolly root record");
			if (memcmp(data + DG_HEADER + 80, binding.bytes, 32))
			{
				pfree(data);
				continue;
			}
			if (snap == SnapshotSelf && ActiveSnapshotSet())
			{
				TransactionId xid = pl_read32(data + DG_HEADER);
				Snapshot	active = GetActiveSnapshot();

				if (!TransactionIdIsCurrentTransactionId(xid) && TransactionIdDidCommit(xid) &&
					IsMVCCSnapshot(active) && XidInMVCCSnapshot(xid, active))
					ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("prolly root advanced after snapshot; retry transaction")));
			}
			if (visible(pl_read32(data + DG_HEADER), pl_read32(data + DG_HEADER + 4), snap))
			{
				if (wanted)
				{
					PLTree		candidate = s->handles;
					PLValue		value;
					uint8		key[8];
					PLSlice		k = {key, 8};

					memcpy(candidate.root.bytes, data + DG_HEADER + 40, 32);
					pl_write64(key, *wanted);
					dg_check(pl_get(&candidate, k, &value));
					if (!value.present)
					{
						pfree(data);
						continue;
					}
				}
				found_root = true;
				memcpy(s->content.root.bytes, data + DG_HEADER + 8, 32);
				memcpy(s->handles.root.bytes, data + DG_HEADER + 40, 32);
				s->nextid = pl_read64(data + DG_HEADER + 72);
			}
		}
		else
			elog(ERROR, "unknown prolly record type");
		pfree(data);
	}
	if (assembly)
		pfree(assembly);		/* An aborted partial append is unreachable. */
	s->has_root = found_root;
	if (!found_root && use_base)
		dg_branch_base(rel, s);
	dg_check(pl_validate(&s->content));
	dg_check(pl_validate(&s->handles));
	return s;
}
DGState *
dg_load(Relation rel, Snapshot snap)
{
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
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		return dg_storage_am_load_tid(rel, id);

	return load_state(rel, SnapshotSelf, &id, dg_relation_branch(rel), true, false);
}

/* Internal page-reader entrypoint; only dg_resource.c supplies this descriptor. */
DGState *
dg_load_bound(Relation rel, Snapshot snap, const char *branch)
{
	Assert(CatalogViewObjectContext(RelationGetRelid(rel)));
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
append(Relation rel, const uint8 *data, Size length)
{
	Buffer		buf = ReadBufferExtended(rel, MAIN_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	GenericXLogState *wal;
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	wal = GenericXLogStart(rel);
	page = GenericXLogRegisterBuffer(wal, buf, GENERIC_XLOG_FULL_IMAGE);
	PageInit(page, BLCKSZ, 0);
	if (PageAddItem(page, (Item) data, length, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "prolly record does not fit page");
	GenericXLogFinish(wal);
	UnlockReleaseBuffer(buf);
}
static void
save_state(Relation rel, DGState *s, CommandId cid, const char *branch)
{

	size_t		i;
	uint8		data[DG_HEADER + DG_PART];
	PLHash		binding;
	PLSlice		branchbytes = {(const uint8 *) branch, strlen(branch)};

	if (!pl_hash_bytes(branchbytes, &binding))
		elog(ERROR, "branch hash failed");

	for (i = 0; i < s->store->capacity; i++)
	{
		PLNode	   *node;

		for (node = s->store->buckets[i]; node; node = node->next)
		{
			size_t		offset = 0;

			if (hash_search(s->persisted, &node->hash, HASH_FIND, NULL))
				continue;
			while (offset < node->size)
			{
				size_t		length = Min(DG_PART, node->size - offset);

				memset(data, 0, DG_HEADER);
				pl_write32(data, DG_MAGIC);
				pl_write32(data + 4, 1);
				pl_write32(data + 8, node->size);
				pl_write32(data + 12, offset);
				memcpy(data + 16, node->hash.bytes, 32);
				memcpy(data + DG_HEADER, node->data + offset, length);
				append(rel, data, DG_HEADER + length);
				offset += length;
			}
			hash_search(s->persisted, &node->hash, HASH_ENTER, NULL);
		}
	}
	memset(data, 0, DG_HEADER + 112);
	pl_write32(data, DG_MAGIC);
	pl_write32(data + 4, 2);
	pl_write32(data + DG_HEADER, GetCurrentTransactionId());
	pl_write32(data + DG_HEADER + 4, cid);
	memcpy(data + DG_HEADER + 8, s->content.root.bytes, 32);
	memcpy(data + DG_HEADER + 40, s->handles.root.bytes, 32);
	pl_write64(data + DG_HEADER + 72, s->nextid);
	memcpy(data + DG_HEADER + 80, binding.bytes, 32);
	append(rel, data, DG_HEADER + 112);
}
void
dg_save(Relation rel, DGState *s, CommandId cid)
{
	if (CatalogViewObjectContext(RelationGetRelid(rel)))
		dg_storage_require_write(rel);
	save_state(rel, s, cid, dg_relation_branch(rel));
}

/* Only the transactional resource allocator calls this, before publishing any
 * branch binding. WAL contains an empty, branch-tagged root under the caller XID. */
void
dg_initialize_resource(Relation rel, const char *branch)
{
	DGState    *state;

	Assert(CatalogViewObjectContext(RelationGetRelid(rel)));
	Assert(RelationGetNumberOfBlocks(rel) == 0);
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
