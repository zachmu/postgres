/* Database-local operational metadata; native heap and WAL. */
#ifndef PG_DOLT_BACKUP_H
#define PG_DOLT_BACKUP_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_backup_d.h"
CATALOG(pg_dolt_backup,9612,DoltBackupRelationId)
{
#ifdef CATALOG_VARLEN
	text		backupname BKI_FORCE_NOT_NULL;
	text		backupurl BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_backup;
typedef FormData_pg_dolt_backup * Form_pg_dolt_backup;
DECLARE_TOAST(pg_dolt_backup, 9613, 9614);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_backup_pkey, 9615, DoltBackupKeyIndexId, pg_dolt_backup, btree(backupname text_ops));
#endif
