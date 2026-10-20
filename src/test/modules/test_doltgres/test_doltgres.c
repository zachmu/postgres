/*-------------------------------------------------------------------------
 * test_doltgres.c
 *    Phase 1 experiment: branch names mapped to distinct catalog objects.
 *
 * Not a branch catalog service.  Explicit LOAD is required in each backend.
 * Physical namespaces are test fixtures, not a production serialization.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"

PG_MODULE_MAGIC;

static rangevar_projection_hook_type previous_hook = NULL;

static const RangeVar *
project_relation(const RangeVar *relation)
{
	const char *separator;
	const char *branch;
	char	   *database;
	char	   *schema;
	RangeVar   *projected;

	if (previous_hook)
		relation = previous_hook(relation);

	if (relation == NULL)
		elog(ERROR, "relation projection hook returned NULL");

	if (relation->catalogname == NULL)
		return relation;

	separator = strpbrk(relation->catalogname, "/@");
	if (separator == NULL)
		return relation;

	database = pnstrdup(relation->catalogname,
					   separator - relation->catalogname);
	branch = separator + 1;
	if (strcmp(database, get_database_name(MyDatabaseId)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("branch projection requires the current database")));

	if (branch[0] == '\0' || relation->schemaname == NULL ||
		strpbrk(branch, "/@") != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid prototype branch qualifier")));

	/* Length-prefix the branch to avoid branch/schema concatenation aliases. */
	schema = psprintf("dg_%zu_%s_%s", strlen(branch), branch,
					  relation->schemaname);
	if (strlen(schema) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("projected namespace name is too long")));

	/* Never modify a parse tree retained by a prepared statement. */
	projected = (RangeVar *) copyObject(relation);
	projected->catalogname = database;
	projected->schemaname = schema;
	return projected;
}

void
_PG_init(void)
{
	previous_hook = rangevar_projection_hook;
	rangevar_projection_hook = project_relation;
}
