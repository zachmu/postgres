/* Per-database manifest ref storage; internal, heap-backed and WAL logged. */
#ifndef PG_DOLT_REF_H
#define PG_DOLT_REF_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_ref_d.h"
CATALOG(pg_dolt_ref,9105,DoltRefRelationId)
{
#ifdef CATALOG_VARLEN
	text		refname BKI_FORCE_NOT_NULL;
	bytea		reftarget BKI_FORCE_NOT_NULL;
	bytea		reftoken BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_ref;
typedef FormData_pg_dolt_ref * Form_pg_dolt_ref;
DECLARE_TOAST(pg_dolt_ref, 9106, 9107);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_ref_key_index, 9108, DoltRefKeyIndexId, pg_dolt_ref, btree(refname text_ops));
#endif
