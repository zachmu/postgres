/* Branch binding boundary. Future revision-qualified RTEs supply per-reference bindings. */
#ifndef DOLT_BRANCH_H
#define DOLT_BRANCH_H
#include "utils/relcache.h"
extern const char *dg_current_branch(void);
extern const char *dg_relation_branch(Relation rel);
extern void dg_select_branch(const char *name);
extern void dg_guard_branch_ddl(Node *statement);
struct RangeVar;
extern const struct RangeVar *dg_project_relation(const struct RangeVar *relation);
extern bool dg_projected_namespace(Oid namespaceId);
extern void dg_projection_write_check(Relation rel);
extern bool dg_projection_building(void);
extern void dg_projection_relation_check(Oid relationId, int lockmode);
#endif
