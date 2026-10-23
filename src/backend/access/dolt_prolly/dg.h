#ifndef DOLT_PROLLY_PG_H
#define DOLT_PROLLY_PG_H
#include "postgres.h"
#include "access/tableam.h"
#include "access/table.h"
#include "nodes/execnodes.h"
#include "commands/defrem.h"
#include "access/genam.h"
#include "executor/tuptable.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "../../../doltgres/prolly/internal.h"

typedef struct DGState
{
	PLStore    *store;
	bool		has_root;
	PLTree		content;
	PLTree		handles;
	uint64		nextid;
	HTAB	   *persisted;
	MemoryContextCallback cleanup;
} DGState;
typedef struct DGRows
{
	PLEntry    *items;
	size_t		count;
	size_t		capacity;
}			DGRows;
typedef struct DGStorage DGStorage;
extern Oid	dg_storage_allocate(Relation definition, const char *branch);
extern void dg_initialize_resource(Relation rel, const char *branch);
extern void dg_storage_record(Relation source);
extern DGStorage *dg_storage_open(Oid definition, const char *branch, const char *name, bytea *schema, Oid resource, Snapshot snapshot);
extern DGState *dg_storage_load(DGStorage *storage);
extern void dg_storage_close(DGStorage *storage);
extern void dg_storage_lock_branch(Oid resource, const char *branch);
extern void dg_storage_lock_working(const char *branch, Snapshot snapshot);
extern Relation dg_catalog_write_table(const char *branch, const char *name, AclMode permission);
extern Relation dg_storage_enable_read(DGStorage *storage, bytea *base);
extern Relation dg_storage_enable_write(DGStorage *storage, bytea *base);
extern void dg_storage_close_relation(Relation rel);
extern void dg_storage_set_indexes(Relation rel, List *indexes);
extern List *dg_storage_indexes(Relation rel);
extern Relation dg_storage_index(Relation rel, Oid id);
extern const char *dg_storage_branch(Relation rel);
extern void dg_storage_require_write(Relation rel);
extern DGState *dg_storage_am_load(Relation rel, Snapshot snapshot);
extern DGState *dg_storage_am_load_tid(Relation rel, uint64 id);
extern DGState *dg_load_bound_tid(Relation rel, uint64 id, const char *branch);
extern DGState *dg_load_bound(Relation rel, Snapshot snapshot, const char *branch);
extern void dg_check(PLStatus status);
extern PLHash dg_rowhash(PLSlice row);
extern DGState *dg_load(Relation rel, Snapshot snapshot);
extern DGState *dg_catalog_mutation_load(Relation rel, Snapshot snapshot);
extern bool dg_catalog_begin_rewrite(Relation definition);
extern void dg_catalog_finish_rewrite(Relation definition);
extern DGState *dg_catalog_rewrite_state(Relation definition);
extern void dg_decode_mutation(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot);
extern DGState *dg_load_branch(Relation rel, Snapshot snapshot, const char *branch);
extern DGState *dg_load_tid(Relation rel, uint64 id);
extern void dg_save(Relation rel, DGState *state, CommandId cid);
extern void dg_release(DGState *state);
extern void dg_branch_base(Relation rel, DGState *state);
extern bytea *dg_schema_store(const char *kind, const char *json);
extern char *dg_schema_read(bytea *address, const char *kind);
extern void dg_history_handles(DGState *state);
extern bytea *dg_history_export_tree(const PLTree * tree);
extern PLTree dg_history_import(PLStore *store, bytea *address);
extern void dg_history_install(Relation rel, DGState *state, PLTree contents);
extern DGRows dg_rows(const PLTree * tree);
extern void dg_tid(uint64 id, ItemPointer tid);
extern uint64 dg_id(ItemPointer tid);
extern PLSlice dg_encode(Relation rel, TupleTableSlot *slot);
extern HeapTuple dg_decode_descriptor(TupleDesc desc, PLSlice bytes);
extern void dg_decode(Relation rel, PLSlice tuple, ItemPointer tid, TupleTableSlot *slot);
extern void dg_validate_descriptor(TupleDesc desc);
extern void dg_rebuild_indexes(Relation rel);
extern void dg_index_rebuild(Relation table, Relation index);
extern const TableAmRoutine *dg_table_routine(void);
extern pg_noreturn void dg_unsupported(const char *operation);
#endif
