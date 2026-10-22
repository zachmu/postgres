/* Local storage resources, never part of canonical schema identities. */
#ifndef PG_DOLT_STORAGE_H
#define PG_DOLT_STORAGE_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_storage_d.h"
CATALOG(pg_dolt_storage,9114,DoltStorageRelationId)
{
	Oid			storageid BKI_FORCE_NOT_NULL;
	Oid			storagespc BKI_FORCE_NOT_NULL;
	Oid			storagedb BKI_FORCE_NOT_NULL;
	Oid			storagefile BKI_FORCE_NOT_NULL;
	Oid			storageam BKI_FORCE_NOT_NULL;
	char		storagekind BKI_FORCE_NOT_NULL;
	bool		storageimported BKI_FORCE_NOT_NULL;
} FormData_pg_dolt_storage;
typedef FormData_pg_dolt_storage *Form_pg_dolt_storage;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_storage_key_index, 9117, DoltStorageKeyIndexId, pg_dolt_storage, btree(storageid oid_ops));
#endif
