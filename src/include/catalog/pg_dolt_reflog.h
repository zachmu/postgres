/* Database-local operational metadata; native heap and WAL. */
#ifndef PG_DOLT_REFLOG_H
#define PG_DOLT_REFLOG_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_reflog_d.h"
CATALOG(pg_dolt_reflog,9604,DoltReflogRelationId)
{
#ifdef CATALOG_VARLEN
	timestamptz reflogtime BKI_FORCE_NOT_NULL;
	text		reflogname BKI_FORCE_NOT_NULL;
	bytea		reflogtarget BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_reflog;
typedef FormData_pg_dolt_reflog * Form_pg_dolt_reflog;
DECLARE_TOAST(pg_dolt_reflog, 9605, 9606);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_reflog_pkey, 9607, DoltReflogKeyIndexId, pg_dolt_reflog, btree(reflogname text_ops, reflogtime timestamptz_ops, reflogtarget bytea_ops));
#endif
