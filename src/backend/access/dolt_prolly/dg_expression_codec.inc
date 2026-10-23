/* Expression payloads contain SQL and symbolic column names, never serialized
 * executor nodes or local OIDs. Rebinding uses native expression transformation.
 * The first codec deliberately admits only built-in scalar expression families;
 * adding a node requires auditing every identity and datum it contains. */
static bool
schema_expression_check(Node *node, void *context)
{
	if (!node)
		return false;
	switch (nodeTag(node))
	{
		case T_List:
		case T_CaseWhen:
			return expression_tree_walker(node, schema_expression_check, context);
		case T_Var:
			if (((Var *) node)->varno != 1 || ((Var *) node)->varattno <= 0 || ((Var *) node)->varlevelsup)
				unsupported("canonical expression whole-row or external reference");
			break;
		case T_Const:
			break;
		case T_FuncExpr:
			if (((FuncExpr *) node)->funcid >= FirstNormalObjectId)
				unsupported("canonical expression user-defined function");
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			if (((OpExpr *) node)->opno >= FirstNormalObjectId)
				unsupported("canonical expression user-defined operator");
			break;
		case T_ScalarArrayOpExpr:
			if (((ScalarArrayOpExpr *) node)->opno >= FirstNormalObjectId)
				unsupported("canonical expression user-defined operator");
			break;
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_ArrayExpr:
		case T_ArrayCoerceExpr:
		case T_RelabelType:
		case T_CoerceViaIO:
		case T_CollateExpr:
		case T_SQLValueFunction:
			break;
		default:
			unsupported("canonical scalar expression node codec");
	}
	if (exprType(node) >= FirstNormalObjectId)
		unsupported("canonical expression user-defined result type");
	if (!context || exprType(node) != VOIDOID)
	{
		TupleDesc	desc = CreateTemplateTupleDesc(1);

		TupleDescInitEntry(desc, 1, "value", exprType(node), -1, 0);
		dg_validate_descriptor(desc);
		FreeTupleDesc(desc);
	}
	return expression_tree_walker(node, schema_expression_check, context);
}

static void
schema_expression_gucs(void)
{
	set_config_option("TimeZone", "UTC", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("DateStyle", "ISO, YMD", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("IntervalStyle", "postgres", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("extra_float_digits", "3", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("bytea_output", "hex", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("standard_conforming_strings", "on", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
}

static List *
schema_expression_parse(const char *sql)
{
	List	   *result = NIL;
	int			nest = NewGUCNestLevel();

	PG_TRY();
	{
		schema_expression_gucs();
		result = raw_parser(sql, RAW_PARSE_DEFAULT);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return result;
}

static char *
schema_expression_sql(Node *expression, Oid relation)
{
	int			nest = NewGUCNestLevel();
	char	   *result = NULL;

	(void) schema_expression_check(expression, NULL);
	PG_TRY();
	{
		schema_expression_gucs();
		result = deparse_expression(expression, deparse_context_for(get_rel_name(relation), relation), false, false);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
	}
	PG_END_TRY();
	return result;
}

static Node *
schema_expression_bind(CatalogView *view, Oid relation, const char *name,
					   TupleDesc desc, Node *raw, int attribute)
{
	ParseState *pstate = make_parsestate(NULL);
	RelationData definition = {0};
	FormData_pg_class form = {0};
	ParseNamespaceItem *nsitem;
	Node	   *result = NULL;
	int			nest = NewGUCNestLevel();

	/*
	 * Bind column Vars from the supplied canonical descriptor. No physical
	 * relation or partially built relcache entry participates in this parse.
	 */
	definition.rd_id = relation;
	definition.rd_att = desc;
	definition.rd_rel = &form;
	definition.rd_catalogview = view;
	form.relkind = RELKIND_RELATION;
	namestrcpy(&form.relname, name);
	nsitem = addRangeTableEntryForRelation(pstate, &definition, AccessShareLock, NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);
	PG_TRY();
	{
		if (attribute >= 0)
			schema_expression_gucs();
		if (attribute > 0)
		{
			Form_pg_attribute attr = TupleDescAttr(desc, attribute - 1);

			result = cookDefault(pstate, raw, attr->atttypid, attr->atttypmod, NameStr(attr->attname), 0);
		}
		else if (attribute < 0)
		{
			result = transformExpr(pstate, copyObject(raw), EXPR_KIND_ALTER_COL_TRANSFORM);
			assign_expr_collations(pstate, result);
		}
		else
		{
			result = transformExpr(pstate, raw, EXPR_KIND_CHECK_CONSTRAINT);
			result = coerce_to_boolean(pstate, result, "CHECK");
			assign_expr_collations(pstate, result);
		}
		(void) schema_expression_check(result, attribute < 0 ? (void *) pstate : NULL);
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(true, nest);
		free_parsestate(pstate);
	}
	PG_END_TRY();
	return result;
}
