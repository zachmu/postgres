/* Immutable local namespace indirection for writable runtime generations. */
#ifndef PG_DOLT_NAMESPACE_H
#define PG_DOLT_NAMESPACE_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_namespace_d.h"
CATALOG(pg_dolt_namespace,9115,DoltNamespaceRelationId)
{
	Oid			runtimensp;
	Oid			logicalnsp;
} FormData_pg_dolt_namespace;
typedef FormData_pg_dolt_namespace * Form_pg_dolt_namespace;
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_namespace_key_index, 9116, DoltNamespaceKeyIndexId, pg_dolt_namespace, btree(runtimensp oid_ops));
MAKE_SYSCACHE(DOLTNAMESPACE, pg_dolt_namespace_key_index, 4);
#endif
