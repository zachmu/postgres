/* Database-local operational metadata; native heap and WAL. */
#ifndef PG_DOLT_BRANCH_NAMESPACE_CONTROL_H
#define PG_DOLT_BRANCH_NAMESPACE_CONTROL_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_branch_namespace_control_d.h"
CATALOG(pg_dolt_branch_namespace_control,9620,DoltBranchNamespaceControlRelationId)
{
#ifdef CATALOG_VARLEN
	text		database BKI_FORCE_NOT_NULL;
	text		branch BKI_FORCE_NOT_NULL;
	text		user BKI_FORCE_NOT_NULL;
	text		host BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_branch_namespace_control;
typedef FormData_pg_dolt_branch_namespace_control * Form_pg_dolt_branch_namespace_control;
DECLARE_TOAST(pg_dolt_branch_namespace_control, 9621, 9622);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_branch_namespace_control_pkey, 9623, DoltBranchNamespaceControlKeyIndexId, pg_dolt_branch_namespace_control, btree(database text_ops, branch text_ops, user text_ops, host text_ops));
#endif
