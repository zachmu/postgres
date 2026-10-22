/* Per-database manifest object storage; internal, heap-backed and WAL logged. */
#ifndef PG_DOLT_OBJECT_H
#define PG_DOLT_OBJECT_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_object_d.h"
CATALOG(pg_dolt_object,9100,DoltObjectRelationId)
{
#ifdef CATALOG_VARLEN
	bytea		objhash BKI_FORCE_NOT_NULL;
	text		objkind BKI_FORCE_NOT_NULL;
	bytea		objpayload BKI_FORCE_NOT_NULL;
	bytea		objchildren[1] BKI_FORCE_NOT_NULL;
	uuid		objid BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_object;
typedef FormData_pg_dolt_object * Form_pg_dolt_object;
DECLARE_TOAST(pg_dolt_object, 9101, 9102);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_object_id_index, 9109, DoltObjectIdIndexId, pg_dolt_object, btree(objid uuid_ops));
DECLARE_INDEX(pg_dolt_object_key_index, 9103, DoltObjectKeyIndexId, pg_dolt_object, btree(objhash bytea_ops));
#endif
