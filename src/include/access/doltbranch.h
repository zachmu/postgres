/* Branch binding boundary. Future revision-qualified RTEs supply per-reference bindings. */
#ifndef DOLT_BRANCH_H
#define DOLT_BRANCH_H
#include "utils/relcache.h"
extern const char *dg_current_branch(void);
extern const char *dg_relation_branch(Relation rel);
extern void dg_select_branch(const char *name);
extern void dg_guard_branch_ddl(Node *statement);
extern void dg_branch_require_unused(const char *name);
#endif
