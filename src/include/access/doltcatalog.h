/* Read-only versioned catalog provider. No persistent projection objects. */
#ifndef DOLT_CATALOG_H
#define DOLT_CATALOG_H
#include "access/tupdesc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/extensible.h"
struct ParseState;
struct CatalogView;
extern bool dg_catalog_active(void);
extern bool dg_catalog_create_routine(struct ParseState *pstate, Node *statement);
extern bool dg_catalog_creating_routine(void);
extern Oid	dg_catalog_routine_namespace(Oid namespaceid);
extern Oid	dg_catalog_store_routine(HeapTuple tuple, TupleDesc desc);
extern struct CatalogView *dg_catalog_session_view(void);
extern bool dg_catalog_lookup_type(const char *schema, const char *name, bool temp_ok, Oid *result);
extern bool dg_catalog_type_visible(Oid id, const char *name, bool *visible);
extern bool dg_catalog_relation_visible(Oid id, const char *name, bool *visible);
extern void dg_catalog_rewrite_relation(RangeTblEntry *rte);
extern bool dg_catalog_alter(Node *statement);
extern bool dg_catalog_lock_relation(Oid id, LOCKMODE mode, bool nowait);
extern bool dg_catalog_lookup_relation(const RangeVar *rv, bool missing_ok, Oid *result);
extern Relation dg_catalog_target(struct ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte);
extern Relation dg_catalog_copy_relation(Relation definition, bool from);
extern Relation dg_catalog_executor_target(RangeTblEntry *rte, CmdType command);
extern void dg_storage_close_relation(Relation rel);
extern Relation dg_storage_index(Relation rel, Oid id);
extern struct CatalogView *dg_catalog_view(const char *database);
extern bool dg_catalog_creating_relation(Relation rel);
extern void dg_catalog_create_scope(bool enter);
extern void dg_catalog_created(Oid id);
extern bool dg_catalog_create_index(Node *statement);
extern bool dg_catalog_create(Node *statement, const char *query);
extern bool dg_catalog_create_begin(Node *statement);
extern void dg_catalog_create_finish(CreateStmt *stmt);
extern bool dg_catalog_drop(Node *statement);
extern void dg_catalog_check_relation(Relation rel, const char *branch, Snapshot snapshot);
extern TupleDesc dg_catalog_bind(struct ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte);
extern void dg_catalog_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte);
extern const CustomScanMethods *dg_catalog_scan_methods(void);
extern void dg_catalog_permission(const char *descriptor, Oid user);
#endif
