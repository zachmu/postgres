#include "dg.h"
#include "access/doltcatalog.h"
#include "access/doltbranch.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "catalog/pg_dolt_ref.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "access/heapam.h"
#include "access/htup_details.h"

static char *selected_branch;
static char *pending_branch;
static bool registered;

const char *
dg_current_branch(void)
{
	return selected_branch ? selected_branch : "main";
}

/* Keep the AM's binding entrypoint relation-specific for future qualified RTEs. */
const char *
dg_relation_branch(Relation rel)
{
	return dg_current_branch();
}

static void
branch_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && pending_branch)
	{
		if (selected_branch)
			pfree(selected_branch);
		selected_branch = pending_branch;
		pending_branch = NULL;
	}
	else if (event == XACT_EVENT_ABORT && pending_branch)
	{
		pfree(pending_branch);
		pending_branch = NULL;
	}
}
void
dg_select_branch(const char *name)
{
	if (!registered)
	{
		RegisterXactCallback(branch_xact, NULL);
		registered = true;
	}
	if (pending_branch)
		pfree(pending_branch);
	pending_branch = MemoryContextStrdup(TopMemoryContext, name);
}

/* A branch without local root records starts from its retained history state.
 * Existing relation-local records override this base under the reader snapshot. */
void
dg_branch_base(Relation rel, DGState *state)
{
	MemoryContext caller = CurrentMemoryContext;
	Oid			types[] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		args[] = {CStringGetTextDatum(psprintf("heads/%s", dg_relation_branch(rel))),
		CStringGetTextDatum(get_namespace_name(dg_catalog_namespace(RelationGetNamespace(rel)))),
	CStringGetTextDatum(RelationGetRelationName(rel))};
	bytea	   *address = NULL;
	bool		isnull;

	if (dg_catalog_creating_relation(rel) || !strcmp(dg_relation_branch(rel), "main"))
		return;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "cannot connect branch SPI");
	MemoryContextSwitchTo(caller);
	SPI_execute_with_args("SELECT d.objchildren[2] FROM pg_catalog.pg_dolt_ref r JOIN pg_catalog.pg_dolt_object c ON c.objhash=r.reftarget JOIN pg_catalog.pg_dolt_object db ON db.objhash=c.objchildren[1] CROSS JOIN LATERAL unnest(db.objchildren) ch JOIN pg_catalog.pg_dolt_object d ON d.objhash=ch WHERE r.refname=$1 AND d.objkind IN ('table','index') AND (pg_catalog.convert_from(d.objpayload,'UTF8')::jsonb->>'name')=pg_catalog.format('%I.%I',$2,$3)", 3, types, args, NULL, true, 1);
	MemoryContextSwitchTo(caller);
	if (SPI_processed)
		address = DatumGetByteaPCopy(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	SPI_finish();
	if (!address)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s has no state on branch %s", RelationGetRelationName(rel), dg_relation_branch(rel))));
	state->content = dg_history_import(state->store, address);
	if (rel->rd_rel->relkind == RELKIND_RELATION)
		dg_history_handles(state);
}

/* Catalogs are shared until branch-aware schema binding is implemented. Freeze
 * schema once a second branch exists, rather than silently changing other views. */
void
dg_guard_branch_ddl(Node *statement)
{
	bool		guard,
				found = false;
	Relation	refs;
	TableScanDesc scan;
	HeapTuple	tuple;

	if (!IsUnderPostmaster)
		return;
	guard = GetCommandLogLevel(statement) == LOGSTMT_DDL || IsA(statement, TruncateStmt);
	if (!guard)
		return;
	LockDatabaseObject(DoltRefRelationId, 0, 0, ShareLock);
	refs = table_open(DoltRefRelationId, AccessShareLock);
	scan = table_beginscan_catalog(refs, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
	{
		bool		isnull;
		Datum		value = heap_getattr(tuple, Anum_pg_dolt_ref_refname, RelationGetDescr(refs), &isnull);
		char	   *name = TextDatumGetCString(value);

		if (!strncmp(name, "views/", 6))
			found = true;
		pfree(name);
		if (found)
			break;
	}
	table_endscan(scan);
	table_close(refs, NoLock);
	if (found)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("schema changes with multiple branches are not supported yet"), errhint("This slice shares schema catalogs and supports data-only branch changes.")));
}
