/* Database-local operational metadata; native heap and WAL. */
#ifndef PG_DOLT_REMOTE_H
#define PG_DOLT_REMOTE_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_remote_d.h"
CATALOG(pg_dolt_remote,9600,DoltRemoteRelationId)
{
#ifdef CATALOG_VARLEN
	text		remotename BKI_FORCE_NOT_NULL;
	text		remoteurl BKI_FORCE_NOT_NULL;
	jsonb		remotefetch BKI_FORCE_NOT_NULL;
	jsonb		remoteparams BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_remote;
typedef FormData_pg_dolt_remote * Form_pg_dolt_remote;
DECLARE_TOAST(pg_dolt_remote, 9601, 9602);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_remote_pkey, 9603, DoltRemoteKeyIndexId, pg_dolt_remote, btree(remotename text_ops));
#endif
