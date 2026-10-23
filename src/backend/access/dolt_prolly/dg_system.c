/* Dolt system tables: the dolt namespace and dolt_-prefixed aliases.
 *
 * Doltgres exposes version-control system tables both in an implicit "dolt"
 * schema (dolt.branches) and with a dolt_ prefix in every schema
 * (dolt_branches, public.dolt_branches). The dolt schema and its views are
 * created by initdb; the pg_catalog aliases are found by the ordinary search
 * path. A schema-qualified alias resolves here when the schema exists but
 * holds no relation of that name. */
#include "dg.h"
#include "access/doltsystem.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace_d.h"
#include "utils/lsyscache.h"

bool		dolt_show_system_tables = false;

/* The dolt schema holds built-in definitions, like pg_catalog. */
bool
dg_system_namespace(const char *name)
{
	return name && (!strcmp(name, "pg_catalog") || !strcmp(name, "information_schema") || !strcmp(name, "dolt"));
}

/* Resolve schema.dolt_<name> to the pg_catalog alias when the schema exists.
 * A relation the user created with that name in the schema takes precedence;
 * callers only reach this after their own lookup failed. */
Oid
dg_system_relation(const RangeVar *rv)
{
	Oid			id;

	if (!rv->schemaname || strncmp(rv->relname, "dolt_", 5) != 0)
		return InvalidOid;
	if (rv->relpersistence == RELPERSISTENCE_TEMP)
		return InvalidOid;
	if (!OidIsValid(get_namespace_oid(rv->schemaname, true)))
		return InvalidOid;
	id = get_relname_relid(rv->relname, PG_CATALOG_NAMESPACE);
	if (!OidIsValid(id) || get_rel_relkind(id) != RELKIND_VIEW)
		return InvalidOid;
	return id;
}
