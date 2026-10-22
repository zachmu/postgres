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
extern bool dg_catalog_create_begin(Node *statement);
extern void dg_catalog_create_finish(CreateStmt *stmt);
extern bool dg_catalog_drop(Node *statement);
extern void dg_catalog_check_relation(Relation rel, const char *branch, Snapshot snapshot);
extern TupleDesc dg_catalog_bind(struct ParseState *pstate, const RangeVar *rv, RangeTblEntry *rte);
extern void dg_catalog_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte);
extern const CustomScanMethods *dg_catalog_scan_methods(void);
extern void dg_catalog_permission(const char *descriptor, Oid user);
#endif
