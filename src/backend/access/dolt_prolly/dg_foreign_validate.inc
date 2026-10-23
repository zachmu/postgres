/* Root recombination bypasses DML triggers. Check the complete resulting graph
 * after all tables are installed, while their branch writer locks remain held.
 * Whole-database restore/fast-forward uses an already retained root instead.
 * Until Dolt FK violation records are supported, violations abort publication;
 * this also conservatively checks pre-existing NOT VALID constraints. */
static void
validate_working_foreign_keys(const char *branch)
{
	bytea	   *database = read_ref(psprintf("working/%s", branch)).target;
	CatalogView *view;
	CatalogView *previous;
	const char *previous_branch;
	List	   *constraints;
	ListCell   *lc;
	int			depth = catalog_api_depth;
	Oid			user;
	int			security;

	if (!database || !read_ref("views/enabled").target)
		return;
	view = provider_view(database);
	CatalogViewEnsureLoaded(view);
	constraints = CatalogViewTuples(view, ConstraintRelationId);
	previous = CatalogViewSwitch(view);
	previous_branch = dg_switch_execution_branch(branch);
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		catalog_api_depth = 0;
		foreach(lc, constraints)
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT((HeapTuple) lfirst(lc));
			Relation	fk;
			Relation	pk;
			Trigger		trigger = {0};

			if (con->contype != CONSTRAINT_FOREIGN)
				continue;
			fk = dg_catalog_trigger_relation(con->conrelid, branch);
			pk = dg_catalog_trigger_relation(con->confrelid, branch);
			dg_catalog_foreign_lock(fk);
			dg_catalog_foreign_lock(pk);
			trigger.tgconstraint = con->oid;
			trigger.tgconstrrelid = con->confrelid;
			trigger.tgconstrindid = con->conindid;
			trigger.tgname = NameStr(con->conname);
			if (!RI_Initial_Check(&trigger, fk, pk))
				elog(ERROR, "cannot validate foreign key root publication");
			dg_storage_close_relation(pk);
			dg_storage_close_relation(fk);
		}
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		catalog_api_depth = depth;
		dg_switch_execution_branch(previous_branch);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}
