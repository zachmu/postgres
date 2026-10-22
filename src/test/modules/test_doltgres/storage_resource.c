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

/* Exercise the native table-AM callbacks with an explicitly bound descriptor.
 * This fixture is not SQL INSERT/UPDATE/DELETE integration. */
#include "access/xact.h"
#include "executor/executor.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/acl.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(test_storage_write);
Datum
test_storage_write(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *operation = text_to_cstring(PG_GETARG_TEXT_PP(2));
	AclMode		permission;
	Relation	rel;
	TupleTableSlot *slot;
	Datum	   *values;
	bool	   *nulls;
	int			n;
	ItemPointerData tid;
	TM_FailureData failure;
	LockTupleMode mode;
	TU_UpdateIndexes indexes;
	uint64		result;
	EState	   *estate;
	ResultRelInfo info;

	if (!strcmp(operation, "insert"))
		permission = ACL_INSERT;
	else if (!strcmp(operation, "update"))
		permission = ACL_UPDATE;
	else if (!strcmp(operation, "delete"))
		permission = ACL_DELETE;
	else
		elog(ERROR, "unknown test operation");
	rel = dg_catalog_write_table(branch, name, permission);
	dg_tid(PG_GETARG_INT64(4), &tid);
	slot = table_slot_create(rel, NULL);
	if (permission != ACL_DELETE)
	{
		deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), TEXTOID, -1, false, TYPALIGN_INT,
						  &values, &nulls, &n);
		if (n != RelationGetDescr(rel)->natts)
			elog(ERROR, "test value count does not match canonical definition");
		for (int i = 0; i < n; i++)
		{
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);
			Oid			input,
						param;

			getTypeInputInfo(att->atttypid, &input, &param);
			slot->tts_isnull[i] = nulls[i];
			slot->tts_values[i] = nulls[i] ? (Datum) 0 : OidInputFunctionCall(input, TextDatumGetCString(values[i]), param, att->atttypmod);
		}
		ExecStoreVirtualTuple(slot);
		estate = CreateExecutorState();
		InitResultRelInfo(&info, rel, 0, NULL, 0);
		ExecConstraints(&info, slot, estate);
		FreeExecutorState(estate);
	}
	if (permission == ACL_INSERT)
		table_tuple_insert(rel, slot, GetCurrentCommandId(true), 0, NULL);
	else if (permission == ACL_UPDATE)
		table_tuple_update(rel, &tid, slot, GetCurrentCommandId(true), GetActiveSnapshot(), InvalidSnapshot, true, &failure, &mode, &indexes);
	else
		table_tuple_delete(rel, &tid, GetCurrentCommandId(true), GetActiveSnapshot(), InvalidSnapshot, true, &failure, false);
	result = permission == ACL_DELETE ? 0 : dg_id(&slot->tts_tid);
	ExecDropSingleTupleTableSlot(slot);
	dg_storage_close_relation(rel);
	PG_RETURN_INT64(result);
}
