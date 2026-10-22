/* Available local runtime resources, independent of current branch bindings. */
#ifndef PG_DOLT_GENERATION_H
#define PG_DOLT_GENERATION_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_generation_d.h"
CATALOG(pg_dolt_generation,9117,DoltGenerationRelationId)
{
	Oid			genrelid BKI_FORCE_NOT_NULL;
#ifdef CATALOG_VARLEN
	text		genname BKI_FORCE_NOT_NULL;
	bytea		genschema BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_generation;
typedef FormData_pg_dolt_generation * Form_pg_dolt_generation;
DECLARE_TOAST(pg_dolt_generation, 9118, 9119);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_generation_key_index, 9114, DoltGenerationKeyIndexId, pg_dolt_generation, btree(genrelid oid_ops));
#endif
