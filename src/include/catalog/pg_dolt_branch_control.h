/* Database-local operational metadata; native heap and WAL. */
#ifndef PG_DOLT_BRANCH_CONTROL_H
#define PG_DOLT_BRANCH_CONTROL_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_branch_control_d.h"
CATALOG(pg_dolt_branch_control,9616,DoltBranchControlRelationId)
{
#ifdef CATALOG_VARLEN
	text		database BKI_FORCE_NOT_NULL;
	text		branch BKI_FORCE_NOT_NULL;
	text		user BKI_FORCE_NOT_NULL;
	text		host BKI_FORCE_NOT_NULL;
	text		permissions BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_branch_control;
typedef FormData_pg_dolt_branch_control * Form_pg_dolt_branch_control;
DECLARE_TOAST(pg_dolt_branch_control, 9617, 9618);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_branch_control_pkey, 9619, DoltBranchControlKeyIndexId, pg_dolt_branch_control, btree(database text_ops, branch text_ops, user text_ops, host text_ops));
#endif
