/* Exercise CREATE's storage primitive without adding a customer-facing API. */
#include "postgres.h"
#include "../../../backend/access/dolt_prolly/dg.h"
#include "access/relation.h"
#include "utils/builtins.h"
#include "fmgr.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_storage_allocate);
Datum
test_storage_allocate(PG_FUNCTION_ARGS)
{
	Relation	definition = relation_open(PG_GETARG_OID(0), AccessShareLock);
	Oid			resource = dg_storage_allocate(definition, text_to_cstring(PG_GETARG_TEXT_PP(1)));

	relation_close(definition, AccessShareLock);
	PG_RETURN_OID(resource);
}
