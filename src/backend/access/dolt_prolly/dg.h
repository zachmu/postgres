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

/*
 * A relation's trees as one caller sees them. States from dg_load share the
 * transaction's working store, which loads missing nodes from the chunk store;
 * other states own a private store. A table state has three trees: content
 * (row digest -> count + row), handles (handle -> row digest) and keys
 * (row digest + handle -> present). An index state uses only content.
 */
typedef struct DGState
{
	PLStore    *store;
	bool		has_root;
	PLTree		content;
	PLTree		handles;
	PLTree		keys;
	uint64		nextid;			/* next handle, for private states only */
	bool		shared;			/* store is the transaction's working store */
	bool		based;			/* derived from committed roots */
	PLHash		base[3];		/* those committed content, handle, key roots */
	MemoryContextCallback cleanup;
} DGState;

/* Size of a key-to-handle tree key: row digest followed by the handle. */
#define DG_KEY_SIZE (PL_HASH_SIZE + 8)

/* Handles below this are derived from content; the counter allocates above it. */
#define DG_DERIVED_HANDLES (UINT64CONST(1) << 40)
typedef struct DGRows
{
	PLEntry    *items;
	size_t		count;
	size_t		capacity;
}			DGRows;
typedef struct DGStorage DGStorage;
extern struct _SPI_plan *dg_manifest_plan(const char *sql, int nargs, Oid *types);
extern bool dg_object_read(bytea *hash, const char *kind, char **actual, bytea **payload, struct ArrayType **children);
extern Oid	dg_storage_allocate(Relation definition, const char *branch);
extern void dg_initialize_resource(Relation rel, const char *branch);
extern void dg_storage_record(Relation source);
extern DGStorage *dg_storage_open(Oid definition, const char *branch, const char *name, bytea *schema, Oid resource, Snapshot snapshot);
extern DGState *dg_storage_load(DGStorage *storage);
extern void dg_storage_close(DGStorage *storage);
extern void dg_storage_lock_branch(Oid resource, const char *branch, LOCKMODE mode);
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
extern bool dg_storage_bound(Relation rel);
extern DGState *dg_storage_am_load(Relation rel, Snapshot snapshot);
extern DGState *dg_storage_am_load_tid(Relation rel, uint64 id);
extern DGState *dg_load_bound_tid(Relation rel, uint64 id, const char *branch);
extern DGState *dg_load_bound(Relation rel, Snapshot snapshot, const char *branch);
extern void dg_check(PLStatus status);
extern PLHash dg_rowhash(PLSlice row);
extern DGState *dg_load(Relation rel, Snapshot snapshot);
extern DGState *dg_catalog_mutation_load(Relation rel, Snapshot snapshot);
extern void dg_catalog_mutation_release(DGState *state);
extern bool dg_catalog_begin_rewrite(Relation definition);
extern void dg_catalog_finish_rewrite(Relation definition);
extern DGState *dg_catalog_rewrite_state(Relation definition);
extern void dg_decode_mutation(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot);
extern DGState *dg_load_branch(Relation rel, Snapshot snapshot, const char *branch);
extern DGState *dg_load_tid(Relation rel, uint64 id);
extern void dg_save(Relation rel, DGState *state, CommandId cid);
extern uint64 dg_allocate_handle(Relation rel);
extern Oid	dg_identity_lookup(const char *branch, char kind, const char *name);
extern bool dg_identity_describe(Oid oid, char **branch, char *kind, char **name);
extern Oid	dg_identity_ensure(const char *branch, char kind, const char *name);
extern void dg_identity_invalidate_branch(const char *branch);
extern void dg_identity_delete_branch(const char *branch);
extern void dg_identity_sync(const char *branch, List *members, List *tables);
extern void dg_note_modified(Relation rel, const char *branch, uint64 handle, CommandId cid);
extern bool dg_row_modified(Relation rel, const char *branch, uint64 handle, CommandId *cid);
extern TM_Result dg_record_lock(Relation rel, const char *branch, int64 id, LockTupleMode mode,
								LockWaitPolicy wait, bool follow, TM_FailureData *tmfd, int64 *next);
extern TM_Result dg_record_update(Relation rel, const char *branch, int64 id, int64 next_id,
								  bool key_change, bool wait, TM_FailureData *tmfd, int64 *next);
extern TM_Result dg_record_delete(Relation rel, const char *branch, int64 id, bool wait,
								  bool changing_part, TM_FailureData *tmfd, int64 *next);
extern void dg_handle_key(PLHash digest, uint64 handle, uint8 *key);

/* A command's pending edits to one relation's trees; see dg_batch.inc. */
typedef struct DGBatch DGBatch;
extern DGBatch *dg_batch(Relation rel, CommandId cid);
extern DGBatch *dg_batch_current(Relation rel);
extern DGState *dg_batch_state(DGBatch *batch);
extern void dg_batch_insert(DGBatch *batch, PLSlice row, uint64 handle);
extern void dg_batch_index(DGBatch *batch, PLSlice key, size_t prefix, PLHash digest, PLSlice row, int64 delta);
extern void dg_batch_index_prefix(DGBatch *batch, PLSlice prefix, List **added, List **removed);
extern bool dg_batch_handle_digest(DGBatch *batch, uint64 handle, PLHash *digest);
extern bool dg_batch_row(DGBatch *batch, PLHash digest, PLSlice *row);
extern int	dg_batch_handles(DGBatch *batch, PLHash digest, uint64 **handles);
extern void dg_batch_finish(Relation rel);
extern DGState *dg_batch_load(Relation rel);
extern PLStore *dg_working_store(void);
extern void dg_collect(void);
extern bool dg_version_call_active(void);
extern bool dg_native_rewrite_active(void);
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
/* A prolly relation holding rows: a table, or a materialized view's table. */
#define DG_ROWS_KIND(relkind) ((relkind) == RELKIND_RELATION || (relkind) == RELKIND_MATVIEW)

extern PLSlice dg_encode_value(Oid type, Datum value);
extern Datum dg_decode_value(Oid type, PLSlice bytes);

typedef struct DGConversion
{
	Oid			from;
	Oid			to;
	struct ParamListInfoData *params;
	struct ExprState *state;
	struct ExprContext *context;
} DGConversion;

extern DGConversion *dg_conversion_begin(Oid from, Oid to, int32 typmod);
extern PLSlice dg_conversion_apply(DGConversion *conversion, PLSlice bytes);
extern PLSlice dg_recode_label(Oid type, PLSlice bytes, Oid target, const char *old, const char *renamed);
extern PLSlice dg_recode_composite(Oid type, PLSlice bytes, Oid target, int count, const int *mapping);
extern bool dg_key_ordered(Oid type);
extern PLSlice dg_key_value(Oid type, Datum value);
extern PLSlice dg_key_field(Oid type, PLSlice bytes);
extern void dg_index_apply(Relation table, PLSlice row, int64 delta, uint64 handle);
extern void dg_index_check_unique(Relation table, Relation index, ItemPointer tid, Datum *values, bool *isnull);
extern void dg_index_validate(Relation table, Relation index);
extern HeapTuple dg_decode_descriptor(TupleDesc desc, PLSlice bytes);
extern void dg_decode(Relation rel, PLSlice tuple, ItemPointer tid, TupleTableSlot *slot);
extern void dg_validate_descriptor(TupleDesc desc);
extern void dg_rebuild_indexes(Relation rel);
extern void dg_index_rebuild(Relation table, Relation index);
extern const TableAmRoutine *dg_table_routine(void);
extern pg_noreturn void dg_unsupported(const char *operation);

/*
 * Identifier quoting for canonical names and SQL. Unlike quote_identifier,
 * these ignore quote_all_identifiers: canonical bytes never depend on a GUC.
 */
extern const char *dg_quote_identifier(const char *ident);
extern char *dg_qualified_name(const char *qualifier, const char *ident);
#endif
