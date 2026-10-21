/* Local, rebuildable runtime identities for retained canonical definitions. */
#ifndef PG_DOLT_PROJECTION_H
#define PG_DOLT_PROJECTION_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_projection_d.h"
CATALOG(pg_dolt_projection,9170,DoltProjectionRelationId)
{
	Oid			runtime_namespace BKI_FORCE_NOT_NULL;
#ifdef CATALOG_VARLEN
	bytea		commit_hash BKI_FORCE_NOT_NULL;
	text		logical_namespace BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_projection;
typedef FormData_pg_dolt_projection * Form_pg_dolt_projection;
DECLARE_TOAST(pg_dolt_projection, 9171, 9172);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_projection_key_index, 9173, DoltProjectionKeyIndexId, pg_dolt_projection, btree(commit_hash bytea_ops, logical_namespace text_ops));
DECLARE_UNIQUE_INDEX(pg_dolt_projection_namespace_index, 9174, DoltProjectionNamespaceIndexId, pg_dolt_projection, btree(runtime_namespace oid_ops));
#endif
