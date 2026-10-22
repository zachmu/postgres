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
extern void dg_check(PLStatus status);
extern PLHash dg_rowhash(PLSlice row);
extern DGState *dg_load(Relation rel, Snapshot snapshot);
extern DGState *dg_load_branch(Relation rel, Snapshot snapshot, const char *branch);
extern DGState *dg_load_tid(Relation rel, uint64 id);
extern void dg_save(Relation rel, DGState *state, CommandId cid);
extern void dg_release(DGState *state);
extern void dg_branch_base(Relation rel, DGState *state);
extern bytea *dg_schema_store(const char *kind, const char *json);
extern char *dg_schema_read(bytea *address, const char *kind);
extern void dg_history_handles(DGState *state);
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
