/* Local runtime bindings. Never included in canonical manifest hashes. */
#ifndef PG_DOLT_BINDING_H
#define PG_DOLT_BINDING_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_binding_d.h"
CATALOG(pg_dolt_binding,9110,DoltBindingRelationId)
{
	Oid			bindrelid BKI_FORCE_NOT_NULL;
#ifdef CATALOG_VARLEN
	text		bindbranch BKI_FORCE_NOT_NULL;
	text		bindname BKI_FORCE_NOT_NULL;
	bytea		bindschema BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_binding;
typedef FormData_pg_dolt_binding * Form_pg_dolt_binding;
DECLARE_TOAST(pg_dolt_binding, 9111, 9112);
DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_binding_key_index, 9113, DoltBindingKeyIndexId, pg_dolt_binding, btree(bindbranch text_ops, bindname text_ops));
#endif
