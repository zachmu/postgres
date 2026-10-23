/* Dolt system table and namespace resolution hooks. */
#ifndef DOLT_SYSTEM_H
#define DOLT_SYSTEM_H
#include "nodes/primnodes.h"

extern PGDLLIMPORT bool dolt_show_system_tables;

extern Oid	dg_system_relation(const RangeVar *rv);
extern bool dg_system_namespace(const char *name);
#endif
