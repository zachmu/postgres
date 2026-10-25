/* Database-local operational metadata; native heap and WAL. */
#ifndef PG_DOLT_REBASE_H
#define PG_DOLT_REBASE_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_rebase_d.h"
CATALOG(pg_dolt_rebase,9608,DoltRebaseRelationId)
{
	float8		rebase_order BKI_FORCE_NOT_NULL;
#ifdef CATALOG_VARLEN
	text		rebasebranch BKI_FORCE_NOT_NULL;
	text		action BKI_FORCE_NOT_NULL;
	text		commit_hash BKI_FORCE_NOT_NULL;
	text		commit_message;
#endif
} FormData_pg_dolt_rebase;
typedef FormData_pg_dolt_rebase * Form_pg_dolt_rebase;
DECLARE_TOAST(pg_dolt_rebase, 9609, 9610);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_rebase_pkey, 9611, DoltRebaseKeyIndexId, pg_dolt_rebase, btree(rebasebranch text_ops, rebase_order float8_ops));
#endif
