/* Canonical database schemas decoded into native, immutable catalog tuples.
 * This provider never copies a live user relation's definition. Only bootstrap
 * catalog definitions/types and global role identities come from local catalogs.
 */
typedef struct DGProvider
{
	char	   *branch;			/* branch head; NULL for a revision */
	char	   *database;
	CatalogView *view;
	uint64		generation;		/* identity generation a branch view binds */
	struct DGProvider *next;
} DGProvider;
static DGProvider *providers;

/*
 * A branch view binds its objects' durable OIDs. Any change to the identity
 * catalog may rebind a name, so it retires every branch view built before.
 */
static uint64 identity_generation = 1;
static bool identity_callback;

static void
provider_identities_changed(Datum arg, Oid relid)
{
	if (!OidIsValid(relid) || relid == DoltIdentityRelationId)
		identity_generation++;
}

/* Whether |entry| may serve lookups of |branch|'s views, or revision views. */
static bool
provider_current(DGProvider *entry, const char *branch)
{
	if (!branch)
		return !entry->branch;
	return entry->branch && !strcmp(entry->branch, branch) && entry->generation == identity_generation;
}

static const Oid provider_catalogs[] = {
	NamespaceRelationId, RelationRelationId, AttributeRelationId, ProcedureRelationId, LanguageRelationId,
	TypeRelationId, OperatorRelationId, RangeRelationId, DescriptionRelationId, DependRelationId, IndexRelationId, ConstraintRelationId, CastRelationId, StatisticRelationId, StatisticExtRelationId, StatisticExtDataRelationId, PublicationRelRelationId, PublicationNamespaceRelationId, InheritsRelationId, RewriteRelationId, TriggerRelationId, PolicyRelationId, AttrDefaultRelationId, SharedDependRelationId, SecLabelRelationId, InitPrivsRelationId
};

/* Whether branch and revision views supply rows of |catalog|. */
bool
dg_catalog_provides(Oid catalog)
{
	for (int i = 0; i < lengthof(provider_catalogs); i++)
		if (provider_catalogs[i] == catalog)
			return true;
	return false;
}

typedef struct DGProviderRows
{
	Relation	relations[lengthof(provider_catalogs)];
	List	   *rows[lengthof(provider_catalogs)];
} DGProviderRows;

static int
provider_catalog_index(Oid id)
{
	int			i;

	for (i = 0; i < lengthof(provider_catalogs); i++)
		if (provider_catalogs[i] == id)
			return i;
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("revision-qualified catalog %u is not supported", id)));
	return -1;
}

/* The compiler layout of a catalog's non-null fixed fields is its tuple layout.
 * Form a normal full tuple, with the remaining nullable fields initially NULL.
 */
static HeapTuple
provider_fixed(Relation catalog, const void *data, Size size, int nfixed)
{
	TupleDesc	desc = RelationGetDescr(catalog);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc_array(bool, desc->natts);
	Size		offset = 0;
	int			i;
	HeapTuple	result;

	memset(nulls, true, desc->natts * sizeof(bool));
	for (i = 0; i < nfixed; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attlen <= 0)
			elog(ERROR, "variable field in fixed catalog prefix");
		offset = att_align_nominal(offset, attr->attalign);
		if (offset + attr->attlen > size)
			elog(ERROR, "invalid fixed catalog prefix");
		values[i] = fetch_att((const char *) data + offset, attr->attbyval, attr->attlen);
		nulls[i] = false;
		offset += attr->attlen;
	}
	result = heap_form_tuple(desc, values, nulls);
	pfree(values);
	pfree(nulls);
	return result;
}

static HeapTuple
provider_set(Relation catalog, HeapTuple tuple, int attno, Datum value)
{
	TupleDesc	desc = RelationGetDescr(catalog);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc0_array(bool, desc->natts);
	bool	   *replace = palloc0_array(bool, desc->natts);
	HeapTuple	result;

	values[attno - 1] = value;
	replace[attno - 1] = true;
	result = heap_modify_tuple(tuple, desc, values, nulls, replace);
	heap_freetuple(tuple);
	pfree(values);
	pfree(nulls);
	pfree(replace);
	return result;
}

static void
provider_add(DGProviderRows *rows, Oid catalog, const void *data, Size size, int nfixed)
{
	int			i = provider_catalog_index(catalog);

	rows->rows[i] = lappend(rows->rows[i], provider_fixed(rows->relations[i], data, size, nfixed));
}

static ArrayType *
provider_acl(const char *definition)
{
	int			i,
				n;
	Datum	   *items;
	static const char *names[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE", "REFERENCES", "TRIGGER", "USAGE", "CREATE", "MAINTAIN"};
	static const AclMode modes[] = {ACL_SELECT, ACL_INSERT, ACL_UPDATE, ACL_DELETE, ACL_TRUNCATE, ACL_REFERENCES, ACL_TRIGGER, ACL_USAGE, ACL_CREATE, ACL_MAINTAIN};

	run("SELECT v->>0,v->>1,v->>2,(v->>3)::bool FROM jsonb_array_elements(COALESCE($1::jsonb->'acl','[]'::jsonb)) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	n = SPI_processed;
	items = palloc_array(Datum, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		AclItem    *item = palloc0(sizeof(*item));
		char	   *grantor = strcell(i, 1),
				   *grantee = strcell(i, 2),
				   *priv = strcell(i, 3);
		bool		grant = DatumGetBool(cell(i, 4));
		AclMode		mode = 0;
		int			j;

		for (j = 0; j < lengthof(names); j++)
			if (!strcmp(priv, names[j]))
				mode = modes[j];
		if (!mode)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical privilege %s", priv)));
		item->ai_grantor = get_role_oid(grantor, false);
		item->ai_grantee = !strcmp(grantee, "PUBLIC") ? ACL_ID_PUBLIC : get_role_oid(grantee, false);
		ACLITEM_SET_PRIVS_GOPTIONS(*item, mode, grant ? mode : 0);
		items[i] = PointerGetDatum(item);
	}
	/* ACLs require a one-dimensional header even when there are no items. */
	if (!n)
		return (ArrayType *) make_empty_acl();
	return construct_array(items, n, ACLITEMOID, sizeof(AclItem), false, TYPALIGN_INT);
}

static void
provider_description(DGProviderRows *rows, Oid object, Oid catalog, int subid, const char *comment)
{
	FormData_pg_description form = {0};
	int			i = provider_catalog_index(DescriptionRelationId);
	HeapTuple	tuple;

	if (!comment)
		return;
	form.objoid = object;
	form.classoid = catalog;
	form.objsubid = subid;
	tuple = provider_fixed(rows->relations[i], &form, sizeof(form), Anum_pg_description_objsubid);
	tuple = provider_set(rows->relations[i], tuple, Anum_pg_description_description, CStringGetTextDatum(comment));
	rows->rows[i] = lappend(rows->rows[i], tuple);
}

static void
provider_dependency(DGProviderRows *rows, Oid objectclass, Oid object, int subid,
					Oid refclass, Oid ref, int refsubid, char kind)
{
	FormData_pg_depend form = {0};

	form.classid = objectclass;
	form.objid = object;
	form.objsubid = subid;
	form.refclassid = refclass;
	form.refobjid = ref;
	form.refobjsubid = refsubid;
	form.deptype = kind;
	provider_add(rows, DependRelationId, &form, sizeof(form), Natts_pg_depend);
}

static TupleDesc
provider_columns(const char *definition)
{
	TupleDesc	desc;
	char	  **names,
			  **types,
			  **collations;
	int		   *mods;
	bool	   *notnull;
	int			i,
				n;

	run("SELECT v->>'name',v->>'type',(v->>'typmod')::int,CASE WHEN v->'collation' IS NOT NULL AND v->'collation'<>'null'::jsonb THEN quote_ident(v->'collation'->>0)||'.'||quote_ident(v->'collation'->>1) ELSE NULL END,COALESCE((v->>'notnull')::bool,false) FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) ORDER BY ord", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	n = SPI_processed;
	names = palloc_array(char *, Max(n, 1));
	types = palloc_array(char *, Max(n, 1));
	collations = palloc_array(char *, Max(n, 1));
	mods = palloc_array(int, Max(n, 1));
	notnull = palloc_array(bool, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		types[i] = strcell(i, 2);
		mods[i] = DatumGetInt32(cell(i, 3));
		collations[i] = strcell(i, 4);
		notnull[i] = DatumGetBool(cell(i, 5));
	}
	desc = CreateTemplateTupleDesc(n);
	for (i = 0; i < n; i++)
	{
		Oid			type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(types[i]), ObjectIdGetDatum(PG_CATALOG_NAMESPACE));

		if (!OidIsValid(type))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("unknown canonical built-in type %s", types[i])));
		TupleDescInitEntry(desc, i + 1, names[i], type, mods[i], 0);
		if (collations[i])
			TupleDescInitEntryCollation(desc, i + 1, get_collation_oid(stringToQualifiedNameList(collations[i], NULL), false));
		TupleDescAttr(desc, i)->attnotnull = notnull[i];
		TupleDescAttr(desc, i)->attislocal = true;
		{
			char	   *storage,
					   *compression;

			char	   *generated,
					   *identity;

			run("SELECT $1::jsonb->'columns'->$2->>'storage',$1::jsonb->'columns'->$2->>'compression',($1::jsonb->'columns'->$2->>'default') IS NOT NULL,$1::jsonb->'columns'->$2->>'generated',$1::jsonb->'columns'->$2->>'identity'", 2, VC_TYPES(TEXTOID, INT4OID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(i)), NULL, true);
			storage = strcell(0, 1);
			compression = strcell(0, 2);
			generated = strcell(0, 4);
			identity = strcell(0, 5);
			TupleDescAttr(desc, i)->atthasdef = DatumGetBool(cell(0, 3));
			if (generated)
				TupleDescAttr(desc, i)->attgenerated = generated[0];
			if (identity)
				TupleDescAttr(desc, i)->attidentity = identity[0];
			if (storage)
				TupleDescAttr(desc, i)->attstorage = storage[0];
			if (compression)
				TupleDescAttr(desc, i)->attcompression = compression[0];
		}
	}
	dg_validate_descriptor(desc);
	return desc;
}

/* Bootstrap rows are shared. All live user definitions are excluded, even when
 * their names happen to match a retained member. Public is supplied canonically.
 */
static bool
provider_bootstrap(Oid catalog, HeapTuple tuple, TupleDesc desc)
{
	bool		isnull;
	Oid			object = DatumGetObjectId(heap_getattr(tuple, 1, desc, &isnull));

	if (catalog == SharedDependRelationId)
	{
		Form_pg_shdepend dep = (Form_pg_shdepend) GETSTRUCT(tuple);

		return dep->dbid == InvalidOid ||
			(dep->dbid == MyDatabaseId && dep->objid < FirstNormalObjectId);
	}
	if (catalog == ProcedureRelationId)
	{
		Oid			ns = ((Form_pg_proc) GETSTRUCT(tuple))->pronamespace;

		return ns == PG_CATALOG_NAMESPACE || !strcmp(get_namespace_name(ns), "information_schema");
	}
	if (catalog == DependRelationId)
		object = ((Form_pg_depend) GETSTRUCT(tuple))->objid;
	if (catalog == NamespaceRelationId && object == PG_PUBLIC_NAMESPACE)
		return false;
	if (catalog == DescriptionRelationId && ((Form_pg_description) GETSTRUCT(tuple))->classoid == NamespaceRelationId && object == PG_PUBLIC_NAMESPACE)
		return false;
	return object < FirstNormalObjectId;
}

static int
provider_attribute(TupleDesc desc, const char *name)
{
	int			i;

	for (i = 0; i < desc->natts; i++)
		if (!strcmp(NameStr(TupleDescAttr(desc, i)->attname), name))
			return i + 1;
	ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("canonical attribute %s is missing", name)));
	return 0;
}

typedef struct ProviderRoutineReferences
{
	DGProviderRows *rows;
	Oid			classid;
	Oid			objid;
} ProviderRoutineReferences;

/* As recordDependencyOnExpr: an expression depends on the routines it calls. */
static bool
provider_routine_references(Node *node, ProviderRoutineReferences *context)
{
	if (!node)
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid >= FirstDurableObjectId)
		provider_dependency(context->rows, context->classid, context->objid, 0, ProcedureRelationId,
							((FuncExpr *) node)->funcid, 0, DEPENDENCY_NORMAL);
	return expression_tree_walker(node, provider_routine_references, context);
}

static void
provider_expression_dependencies(DGProviderRows *rows, Oid classid, Oid objid, Node *expression)
{
	ProviderRoutineReferences context = {
		.rows = rows,
		.classid = classid,
		.objid = objid,
	};

	(void) provider_routine_references(expression, &context);
}

static void
provider_defaults(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	Oid			relation = CatalogViewLookupObject(view, RelationRelationId, name);
	TupleDesc	desc = provider_columns(definition);

	for (int i = 0; i < desc->natts; i++)
	{
		char	   *sql;
		Node	   *raw,
				   *expression;
		FormData_pg_attrdef form = {0};
		HeapTuple	tuple;
		int			slot = provider_catalog_index(AttrDefaultRelationId);

		/* An identity column reaches its global sequence by dependency. */
		if (TupleDescAttr(desc, i)->attidentity)
		{
			CatalogView *previous = CatalogViewSwitch(NULL);
			List	   *qualified;
			Oid			sequence;

			run("SELECT $1::jsonb->'columns'->$2->>'sequence'", 2, VC_TYPES(TEXTOID, INT4OID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(i)), NULL, true);
			qualified = stringToQualifiedNameList(strcell(0, 1), NULL);
			sequence = get_relname_relid(strVal(lsecond(qualified)), get_namespace_oid(strVal(linitial(qualified)), true));
			CatalogViewSwitch(previous);
			if (OidIsValid(sequence))
				provider_dependency(rows, RelationRelationId, sequence, 0, RelationRelationId, relation, i + 1, DEPENDENCY_INTERNAL);
		}
		if (!TupleDescAttr(desc, i)->atthasdef)
			continue;
		run("SELECT $1::jsonb->'columns'->$2->>'default'", 2, VC_TYPES(TEXTOID, INT4OID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(i)), NULL, true);
		sql = strcell(0, 1);
		raw = schema_expression_raw(sql);
		expression = schema_expression_bind(view, relation, name, desc, raw, i + 1);
		form.oid = CatalogViewBindObject(view, AttrDefaultRelationId, psprintf("%s/%s", name, NameStr(TupleDescAttr(desc, i)->attname)), InvalidOid);
		form.adrelid = relation;
		form.adnum = i + 1;
		tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_attrdef_adnum);
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_attrdef_adbin, CStringGetTextDatum(nodeToString(expression)));
		rows->rows[slot] = lappend(rows->rows[slot], tuple);
		/*
		 * As StoreAttrDefault records: a default belongs to its column, while
		 * a generation expression is part of its column and depends on its
		 * inputs, which then cannot be dropped alone.
		 */
		provider_dependency(rows, AttrDefaultRelationId, form.oid, 0, RelationRelationId, relation, i + 1,
							TupleDescAttr(desc, i)->attgenerated ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);
		provider_expression_dependencies(rows, AttrDefaultRelationId, form.oid, expression);
		if (TupleDescAttr(desc, i)->attgenerated)
		{
			Bitmapset  *inputs = NULL;
			int			input = -1;

			pull_varattnos(expression, 1, &inputs);
			while ((input = bms_next_member(inputs, input)) >= 0)
				if (input + FirstLowInvalidHeapAttributeNumber != i + 1)
					provider_dependency(rows, AttrDefaultRelationId, form.oid, 0, RelationRelationId, relation,
										input + FirstLowInvalidHeapAttributeNumber, DEPENDENCY_NORMAL);
		}
	}
}

#include "dg_foreign_provider.inc"

static void
provider_constraints(DGProviderRows *rows, CatalogView *view, const char *definition, int count, char **definitions, char **kinds)
{
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Oid			relid = CatalogViewLookupObject(view, RelationRelationId, name);
	Oid			ns = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	TupleDesc	desc = provider_columns(definition);
	char	  **names,
			  **defs,
			  **metadata;
	int			n,
				i;

	run("SELECT v->>0,v->>1,(v->2)::text FROM jsonb_array_elements(COALESCE($1::jsonb->'constraints','[]'::jsonb)) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	n = SPI_processed;
	names = palloc_array(char *, Max(n, 1));
	defs = palloc_array(char *, Max(n, 1));
	metadata = palloc_array(char *, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		defs[i] = strcell(i, 2);
		metadata[i] = strcell(i, 3);
	}
	for (i = 0; i < n; i++)
	{
		List	   *parsed = schema_expression_parse(psprintf("ALTER TABLE placeholder ADD CONSTRAINT %s %s", dg_quote_identifier(names[i]), defs[i]));
		Node	   *node = ((RawStmt *) linitial(parsed))->stmt;
		AlterTableStmt *alter;
		Constraint *constraint;
		Node	   *expression = NULL;
		FormData_pg_constraint form = {0};
		ListCell   *lc;
		Datum	   *keys;
		HeapTuple	tuple;
		int			slot = provider_catalog_index(ConstraintRelationId),
					k = 0;

		if (list_length(parsed) != 1 || !IsA(node, AlterTableStmt))
			elog(ERROR, "invalid canonical constraint");
		alter = (AlterTableStmt *) node;
		if (list_length(alter->cmds) != 1 || !IsA(((AlterTableCmd *) linitial(alter->cmds))->def, Constraint))
			elog(ERROR, "invalid canonical constraint command");
		constraint = (Constraint *) ((AlterTableCmd *) linitial(alter->cmds))->def;
		switch (constraint->contype)
		{
			case CONSTR_PRIMARY:
				form.contype = CONSTRAINT_PRIMARY;
				break;
			case CONSTR_UNIQUE:
				form.contype = CONSTRAINT_UNIQUE;
				break;
			case CONSTR_FOREIGN:
				form.contype = CONSTRAINT_FOREIGN;
				constraint->keys = constraint->fk_attrs;
				break;
			case CONSTR_CHECK:
				form.contype = CONSTRAINT_CHECK;
				expression = schema_expression_bind(view, relid, strVal(lsecond(parts)), desc, constraint->raw_expr, 0);
				break;
			case CONSTR_NOTNULL:
				form.contype = CONSTRAINT_NOTNULL;
				break;
			default:
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical constraint %s", names[i])));
		}
		form.oid = CatalogViewBindObject(view, ConstraintRelationId, psprintf("%s/%s", name, names[i]), InvalidOid);
		namestrcpy(&form.conname, names[i]);
		form.connamespace = ns;
		form.conrelid = relid;
		form.condeferrable = constraint->deferrable;
		form.condeferred = constraint->initdeferred;
		form.conenforced = form.contype == CONSTRAINT_CHECK ? constraint->is_enforced : true;
		form.convalidated = !constraint->skip_validation;
		form.conislocal = true;
		form.connoinherit = form.contype == CONSTRAINT_FOREIGN || constraint->is_no_inherit;
		form.confupdtype = ' ';
		form.confdeltype = ' ';
		form.confmatchtype = ' ';
		if (form.contype == CONSTRAINT_PRIMARY || form.contype == CONSTRAINT_UNIQUE)
			form.conindid = CatalogViewLookupObject(view, RelationRelationId, dg_qualified_name(strVal(linitial(parts)), names[i]));
		keys = palloc_array(Datum, Max(desc->natts, 1));
		if (expression)
		{
			Bitmapset  *attributes = NULL;
			int			att = -1;

			pull_varattnos(expression, 1, &attributes);
			while ((att = bms_next_member(attributes, att)) >= 0)
			{
				int			number = att + FirstLowInvalidHeapAttributeNumber;

				keys[k++] = Int16GetDatum(number);
				provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, number, DEPENDENCY_AUTO);
			}
			bms_free(attributes);
			/* Constant checks still depend on their owning relation. */
			if (!k)
				provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, 0, DEPENDENCY_AUTO);
			provider_expression_dependencies(rows, ConstraintRelationId, form.oid, expression);
		}
		foreach(lc, constraint->keys)
		{
			int			att = provider_attribute(desc, strVal(lfirst(lc)));

			keys[k++] = Int16GetDatum(att);
			provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, att, DEPENDENCY_AUTO);
		}
		tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_constraint_conperiod);
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_conkey, PointerGetDatum(construct_array(keys, k, INT2OID, 2, true, TYPALIGN_SHORT)));
		if (expression)
			tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_conbin, CStringGetTextDatum(nodeToString(expression)));
		if (form.contype == CONSTRAINT_FOREIGN)
			tuple = provider_foreign(rows, view, tuple, constraint, desc, metadata[i], count, definitions, kinds);
		rows->rows[slot] = lappend(rows->rows[slot], tuple);
		if (form.conindid)
			provider_dependency(rows, RelationRelationId, form.conindid, 0, ConstraintRelationId, form.oid, 0, DEPENDENCY_INTERNAL);
	}
}

static void
provider_index(DGProviderRows *rows, CatalogView *view, const char *definition, int count, char **definitions, char **kinds)
{
	char	   *sql = json_value(definition, "index_definition");
	List	   *parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	Node	   *node = ((RawStmt *) linitial(parsed))->stmt;
	IndexStmt  *stmt;
	char	   *table;
	TupleDesc	desc = NULL;
	FormData_pg_index form = {0};
	int16	   *keys,
			   *options;
	Oid		   *classes,
			   *collations;
	ListCell   *lc;
	List	   *elements;
	HeapTuple	tuple;
	int			i,
				k = 0,
				slot = provider_catalog_index(IndexRelationId);

	if (list_length(parsed) != 1 || !IsA(node, IndexStmt))
		elog(ERROR, "invalid canonical index definition");
	stmt = (IndexStmt *) node;
	if (stmt->whereClause || stmt->options || strcmp(stmt->accessMethod, "dolt_prolly_index"))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical index properties")));
	if (!stmt->relation->schemaname)
		elog(ERROR, "unqualified canonical index table");
	table = dg_qualified_name(stmt->relation->schemaname, stmt->relation->relname);
	for (i = 0; i < count; i++)
		if (!strcmp(kinds[i], "table") && !strcmp(json_value(definitions[i], "name"), table))
		{
			desc = provider_columns(definitions[i]);
			break;
		}
	if (!desc)
		elog(ERROR, "canonical index table is missing");
	form.indexrelid = CatalogViewLookupObject(view, RelationRelationId, json_value(definition, "name"));
	form.indrelid = CatalogViewLookupObject(view, RelationRelationId, table);
	form.indnkeyatts = list_length(stmt->indexParams);
	elements = list_concat_copy(stmt->indexParams, stmt->indexIncludingParams);
	form.indnatts = list_length(elements);
	form.indisunique = stmt->unique;
	form.indnullsnotdistinct = stmt->nulls_not_distinct;
	form.indimmediate = true;
	form.indisvalid = true;
	form.indisready = true;
	form.indislive = true;
	foreach(lc, rows->rows[provider_catalog_index(ConstraintRelationId)])
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT((HeapTuple) lfirst(lc));

		if (con->conindid == form.indexrelid && (con->contype == CONSTRAINT_PRIMARY || con->contype == CONSTRAINT_UNIQUE))
		{
			form.indisprimary = con->contype == CONSTRAINT_PRIMARY;
			form.indimmediate = !con->condeferrable;
		}
	}
	keys = palloc0_array(int16, form.indnatts);
	options = palloc0_array(int16, form.indnkeyatts);
	classes = palloc0_array(Oid, form.indnkeyatts);
	collations = palloc0_array(Oid, form.indnkeyatts);
	foreach(lc, elements)
	{
		IndexElem  *elem = lfirst_node(IndexElem, lc);
		Form_pg_attribute attr;

		if (!elem->name || elem->expr || elem->opclassopts)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("canonical expression indexes and opclass options are not supported")));
		keys[k] = provider_attribute(desc, elem->name);
		attr = TupleDescAttr(desc, keys[k] - 1);
		if (k < form.indnkeyatts)
		{
			classes[k] = ResolveOpClass(elem->opclass, attr->atttypid, "dolt_prolly_index", DOLT_PROLLY_INDEX_AM_OID);
			collations[k] = elem->collation ? get_collation_oid(elem->collation, false) : attr->attcollation;
			if (elem->ordering == SORTBY_DESC)
				options[k] |= INDOPTION_DESC;
			if (elem->nulls_ordering == SORTBY_NULLS_FIRST || (elem->nulls_ordering == SORTBY_NULLS_DEFAULT && elem->ordering == SORTBY_DESC))
				options[k] |= INDOPTION_NULLS_FIRST;
		}
		provider_dependency(rows, RelationRelationId, form.indexrelid, 0, RelationRelationId, form.indrelid, keys[k], DEPENDENCY_AUTO);
		k++;
	}
	tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_index_indisreplident);
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indkey, PointerGetDatum(buildint2vector(keys, form.indnatts)));
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indclass, PointerGetDatum(buildoidvector(classes, form.indnkeyatts)));
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indcollation, PointerGetDatum(buildoidvector(collations, form.indnkeyatts)));
	tuple = provider_set(rows->relations[slot], tuple, Anum_pg_index_indoption, PointerGetDatum(buildint2vector(options, form.indnkeyatts)));
	rows->rows[slot] = lappend(rows->rows[slot], tuple);
}

#include "dg_routine_provider.inc"

/* Shared role references are derived from canonical owners and ACLs. They
 * remain runtime catalog tuples; local role OIDs never enter schema payloads. */
static void
provider_role_dependency(DGProviderRows *rows, Oid catalog, Oid object,
						 Oid role, char kind)
{
	FormData_pg_shdepend dep = {0};

	if (!OidIsValid(role))
		return;					/* PUBLIC is not a role object */
	dep.dbid = MyDatabaseId;
	dep.classid = catalog;
	dep.objid = object;
	dep.refclassid = AuthIdRelationId;
	dep.refobjid = role;
	dep.deptype = kind;
	provider_add(rows, SharedDependRelationId, &dep, sizeof(dep), Natts_pg_shdepend);
}

static void
provider_role_dependencies(DGProviderRows *rows)
{
	const Oid	catalogs[] = {NamespaceRelationId, RelationRelationId, TypeRelationId, ProcedureRelationId};
	const int	owners[] = {Anum_pg_namespace_nspowner, Anum_pg_class_relowner, Anum_pg_type_typowner, Anum_pg_proc_proowner};
	const int	acls[] = {Anum_pg_namespace_nspacl, Anum_pg_class_relacl, Anum_pg_type_typacl, Anum_pg_proc_proacl};

	for (int i = 0; i < lengthof(catalogs); i++)
	{
		int			slot = provider_catalog_index(catalogs[i]);
		TupleDesc	desc = RelationGetDescr(rows->relations[slot]);
		ListCell   *lc;

		foreach(lc, rows->rows[slot])
		{
			HeapTuple	tuple = lfirst(lc);
			bool		isnull;
			Oid			object = DatumGetObjectId(heap_getattr(tuple, 1, desc, &isnull));
			Oid			owner;
			Datum		acl;

			if (object < FirstDurableObjectId)
				continue;		/* bootstrap objects are shared */
			owner = DatumGetObjectId(heap_getattr(tuple, owners[i], desc, &isnull));
			provider_role_dependency(rows, catalogs[i], object, owner, SHARED_DEPENDENCY_OWNER);
			acl = heap_getattr(tuple, acls[i], desc, &isnull);
			if (!isnull)
			{
				Oid		   *members;
				int			count = aclmembers(DatumGetAclP(acl), &members);

				for (int j = 0; j < count; j++)
					if (members[j] != owner)
						provider_role_dependency(rows, catalogs[i], object, members[j], SHARED_DEPENDENCY_ACL);
				if (members)
					pfree(members);
			}
		}
	}
}

static bool
provider_has_type(DGProviderRows *rows, Oid namespaceid, const char *name)
{
	ListCell   *lc;

	foreach(lc, rows->rows[provider_catalog_index(TypeRelationId)])
	{
		Form_pg_type type = (Form_pg_type) GETSTRUCT((HeapTuple) lfirst(lc));

		if (type->typnamespace == namespaceid && !strcmp(NameStr(type->typname), name))
			return true;
	}
	return false;
}

/*
 * Bind a table, index or row type. On a branch head it takes the object's
 * durable OID and, for relations, its storage resource; a revision's objects
 * take backend-local handles.
 */
static Oid
provider_bind(CatalogView *view, const char *branch, Oid objectclass, char kind,
			  const char *name, const char *identity)
{
	Oid			existing = CatalogViewFindObject(view, objectclass, identity);
	Oid			oid;
	Oid			storage = InvalidOid;

	if (OidIsValid(existing))
		return existing;
	if (branch && OidIsValid(oid = dg_identity_lookup(branch, kind, name)))
	{
		if (kind == DOLT_IDENTITY_RELATION)
		{
			run("SELECT bindrelid FROM pg_catalog.pg_dolt_binding WHERE bindbranch=$1 AND bindname=$2", 2,
				VC_TYPES(TEXTOID, TEXTOID), VC_ARGS(CStringGetTextDatum(branch), CStringGetTextDatum(name)), NULL, true);
			/* A relation without a binding fails when accessed, not here. */
			if (SPI_processed == 1)
				storage = DatumGetObjectId(cell(0, 1));
		}
		CatalogViewBindDurable(view, objectclass, identity, oid, storage);
		return oid;
	}
	return CatalogViewBindObject(view, objectclass, identity, InvalidOid);
}

static void
provider_populate(CatalogView *view, bytea *database, const char *branch)
{
	DGProviderRows rows = {0};
	bytea	  **addresses;
	char	  **kinds,
			  **definitions;
	int			n,
				i,
				j;

	for (i = 0; i < lengthof(provider_catalogs); i++)
	{
		TableScanDesc scan;
		HeapTuple	tuple;

		rows.relations[i] = table_open(provider_catalogs[i], AccessShareLock);
		scan = table_beginscan_catalog(rows.relations[i], 0, NULL);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)))
			if (provider_bootstrap(provider_catalogs[i], tuple, RelationGetDescr(rows.relations[i])))
				rows.rows[i] = lappend(rows.rows[i], heap_copytuple(tuple));
		table_endscan(scan);
	}
	run("SELECT o.objhash,o.objkind FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible o ON o.objhash=ch WHERE d.objhash=$1", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(database)), NULL, true);
	n = SPI_processed;
	addresses = palloc_array(bytea *, Max(n, 1));
	kinds = palloc_array(char *, Max(n, 1));
	definitions = palloc_array(char *, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		addresses[i] = DatumGetByteaPCopy(cell(i, 1));
		kinds[i] = strcell(i, 2);
	}
	for (i = 0; i < n; i++)
	{
		if (!strcmp(kinds[i], "namespace"))
			definitions[i] = metadata(addresses[i], "namespace");
		else if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index") || !strcmp(kinds[i], "routine"))
			definitions[i] = metadata(child(addresses[i], 1), "schema");
		else
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unsupported canonical catalog member %s", kinds[i])));
	}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "namespace"))
		{
			FormData_pg_namespace form = {0};
			int			slot = provider_catalog_index(NamespaceRelationId);
			char	   *name = json_value(definitions[i], "name");
			char	   *owner = json_value(definitions[i], "owner");
			HeapTuple	tuple;

			form.oid = CatalogViewBindObject(view, NamespaceRelationId, name, InvalidOid);
			namestrcpy(&form.nspname, name);
			form.nspowner = get_role_oid(owner, false);
			tuple = provider_fixed(rows.relations[slot], &form, sizeof(form), Anum_pg_namespace_nspowner);
			tuple = provider_set(rows.relations[slot], tuple, Anum_pg_namespace_nspacl, PointerGetDatum(provider_acl(definitions[i])));
			rows.rows[slot] = lappend(rows.rows[slot], tuple);
			provider_description(&rows, form.oid, NamespaceRelationId, 0, json_value(definitions[i], "comment"));
		}
	/* Bind all class identities before resolving cross-object edges. */
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
			(void) provider_bind(view, branch, RelationRelationId, DOLT_IDENTITY_RELATION,
								 json_value(definitions[i], "name"), json_value(definitions[i], "name"));
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
		{
			const char *definition = definitions[i];
			char	   *name = json_value(definition, "name");
			char	   *owner = json_value(definition, "owner");
			List	   *parts = stringToQualifiedNameList(name, NULL);
			FormData_pg_class form = {0};
			TupleDesc	desc = provider_columns(definition);
			bool		index = !strcmp(kinds[i], "index");
			HeapTuple	tuple;
			int			slot = provider_catalog_index(RelationRelationId);

			if (list_length(parts) != 2)
				elog(ERROR, "canonical relation name is not qualified");
			form.oid = CatalogViewFindObject(view, RelationRelationId, name);
			form.relnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
			namestrcpy(&form.relname, strVal(lsecond(parts)));
			form.relowner = get_role_oid(owner, false);
			form.reltype = index ? InvalidOid : provider_bind(view, branch, TypeRelationId, DOLT_IDENTITY_ROWTYPE, name, name);
			form.relam = index ? DOLT_PROLLY_INDEX_AM_OID : DOLT_PROLLY_TABLE_AM_OID;
			form.relkind = index ? RELKIND_INDEX : RELKIND_RELATION;
			form.relpersistence = RELPERSISTENCE_PERMANENT;
			form.relnatts = desc->natts;
			run("SELECT count(*) FROM jsonb_array_elements($1::jsonb->'constraints') v WHERE v->>1 LIKE 'CHECK %'", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
			form.relchecks = DatumGetInt64(cell(0, 1));
			form.reltuples = -1;
			form.relispopulated = true;
			form.relreplident = REPLICA_IDENTITY_DEFAULT;
			run("SELECT jsonb_array_length(COALESCE($1::jsonb->'indexes','[]'::jsonb))>0", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
			form.relhasindex = DatumGetBool(cell(0, 1));
			tuple = provider_fixed(rows.relations[slot], &form, sizeof(form), Anum_pg_class_relminmxid);
			tuple = provider_set(rows.relations[slot], tuple, Anum_pg_class_relacl, PointerGetDatum(provider_acl(definition)));
			rows.rows[slot] = lappend(rows.rows[slot], tuple);
			provider_description(&rows, form.oid, RelationRelationId, 0, json_value(definition, "comment"));
			provider_dependency(&rows, RelationRelationId, form.oid, 0, NamespaceRelationId, form.relnamespace, 0, DEPENDENCY_NORMAL);
			for (j = 0; j < desc->natts; j++)
			{
				Form_pg_attribute attr = TupleDescAttr(desc, j);

				int			aslot = provider_catalog_index(AttributeRelationId);
				HeapTuple	atuple;
				bool		nullvalue;
				Datum		value;

				attr->attrelid = form.oid;
				atuple = provider_fixed(rows.relations[aslot], attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
				run("SELECT (v->>'statistics')::int,ARRAY(SELECT jsonb_array_elements_text(v->'options')) FROM (SELECT $1::jsonb->'columns'->$2 v) s", 2, VC_TYPES(TEXTOID, INT4OID), VC_ARGS(CStringGetTextDatum(definition), Int32GetDatum(j)), NULL, true);
				value = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &nullvalue);
				if (!nullvalue)
					atuple = provider_set(rows.relations[aslot], atuple, Anum_pg_attribute_attstattarget, value);
				value = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &nullvalue);
				if (!nullvalue && ArrayGetNItems(ARR_NDIM(DatumGetArrayTypeP(value)), ARR_DIMS(DatumGetArrayTypeP(value))) > 0)
					atuple = provider_set(rows.relations[aslot], atuple, Anum_pg_attribute_attoptions, value);
				rows.rows[aslot] = lappend(rows.rows[aslot], atuple);
				provider_dependency(&rows, RelationRelationId, form.oid, j + 1, TypeRelationId, attr->atttypid, 0, DEPENDENCY_NORMAL);
			}
			if (!index)
			{
				FormData_pg_type type = {0};
				int			attno;
				char	   *arrayname = makeObjectName("", NameStr(form.relname), NULL);
				int			arraypass = 0;
				char	   *arrayidentity;
				FormData_pg_type array = {0};

				for (;;)
				{
					arrayidentity = dg_qualified_name(strVal(linitial(parts)), arrayname);
					if (!catalog_contains(database, arrayidentity) &&
						!provider_has_type(&rows, form.relnamespace, arrayname))
						break;
					pfree(arrayname);
					arrayname = makeObjectName("", NameStr(form.relname), psprintf("%d", ++arraypass));
				}
				array.oid = provider_bind(view, branch, TypeRelationId, DOLT_IDENTITY_ARRAYTYPE, name, arrayidentity);
				type.typarray = array.oid;

				type.oid = form.reltype;
				type.typname = form.relname;
				type.typnamespace = form.relnamespace;
				type.typowner = form.relowner;
				type.typlen = -1;
				type.typtype = TYPTYPE_COMPOSITE;
				type.typcategory = TYPCATEGORY_COMPOSITE;
				type.typisdefined = true;
				type.typdelim = ',';
				type.typrelid = form.oid;
				type.typinput = F_RECORD_IN;
				type.typoutput = F_RECORD_OUT;
				type.typreceive = F_RECORD_RECV;
				type.typsend = F_RECORD_SEND;
				type.typalign = TYPALIGN_DOUBLE;
				type.typstorage = TYPSTORAGE_EXTENDED;
				type.typtypmod = -1;
				provider_add(&rows, TypeRelationId, &type, sizeof(type), Anum_pg_type_typcollation);
				provider_dependency(&rows, TypeRelationId, type.oid, 0, RelationRelationId, form.oid, 0, DEPENDENCY_INTERNAL);
				namestrcpy(&array.typname, arrayname);
				array.typnamespace = type.typnamespace;
				array.typowner = type.typowner;
				array.typlen = -1;
				array.typtype = TYPTYPE_BASE;
				array.typcategory = TYPCATEGORY_ARRAY;
				array.typisdefined = true;
				array.typdelim = ',';
				array.typelem = type.oid;
				array.typinput = F_ARRAY_IN;
				array.typoutput = F_ARRAY_OUT;
				array.typreceive = F_ARRAY_RECV;
				array.typsend = F_ARRAY_SEND;
				array.typanalyze = F_ARRAY_TYPANALYZE;
				array.typsubscript = F_ARRAY_SUBSCRIPT_HANDLER;
				array.typalign = TYPALIGN_DOUBLE;
				array.typstorage = TYPSTORAGE_EXTENDED;
				array.typtypmod = -1;
				provider_add(&rows, TypeRelationId, &array, sizeof(array), Anum_pg_type_typcollation);
				provider_dependency(&rows, TypeRelationId, array.oid, 0, TypeRelationId, type.oid, 0, DEPENDENCY_INTERNAL);

				for (attno = -1; attno > FirstLowInvalidHeapAttributeNumber; attno--)
				{
					FormData_pg_attribute attr = *SystemAttributeDefinition(attno);

					attr.attrelid = form.oid;
					provider_add(&rows, AttributeRelationId, &attr, sizeof(attr), Anum_pg_attribute_attcollation);
				}
			}
			run("SELECT ord::int,v->>'comment' FROM jsonb_array_elements($1::jsonb->'columns') WITH ORDINALITY a(v,ord) WHERE v->>'comment' IS NOT NULL", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
			for (j = 0; j < SPI_processed; j++)
				provider_description(&rows, form.oid, RelationRelationId, DatumGetInt32(cell(j, 1)), strcell(j, 2));
		}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "routine"))
			provider_routine(&rows, view, definitions[i]);

	/*
	 * Table expressions may call versioned routines, which they name; the
	 * routines and their schemas are visible while those are bound.
	 */
	CatalogViewAddRows(view, NamespaceRelationId, rows.rows[provider_catalog_index(NamespaceRelationId)]);
	CatalogViewAddRows(view, ProcedureRelationId, rows.rows[provider_catalog_index(ProcedureRelationId)]);
	CatalogViewSetPartial(view, true);
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "table"))
		{
			provider_defaults(&rows, view, definitions[i]);
			provider_constraints(&rows, view, definitions[i], n, definitions, kinds);
		}
	for (i = 0; i < n; i++)
		if (!strcmp(kinds[i], "index"))
			provider_index(&rows, view, definitions[i], n, definitions, kinds);
	provider_role_dependencies(&rows);
	for (i = 0; i < lengthof(provider_catalogs); i++)
	{
		if (provider_catalogs[i] != NamespaceRelationId && provider_catalogs[i] != ProcedureRelationId)
			CatalogViewAddRows(view, provider_catalogs[i], rows.rows[i]);
		table_close(rows.relations[i], AccessShareLock);
	}
	CatalogViewSeal(view);
}

static void
provider_reload(CatalogView *view, void *arg)
{
	DGProvider *entry = arg;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	CatalogView *previous = CatalogViewSwitch(NULL);
	MemoryContext caller = CurrentMemoryContext;
	MemoryContext scratch = AllocSetContextCreate(caller, "catalog reload", ALLOCSET_DEFAULT_SIZES);

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *database;

		MemoryContextSwitchTo(scratch);
		if (!ActiveSnapshotSet())
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			pushed = true;
		}
		nest = begin_api();
		database = unhex(entry->database);
		(void) metadata(database, "database");
		provider_populate(view, database, entry->branch);
		end_api(nest);
	}
	PG_FINALLY();
	{
		if (pushed)
			PopActiveSnapshot();
		MemoryContextSwitchTo(caller);
		MemoryContextDelete(scratch);
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
}

static TransactionId
provider_creator(bytea *database)
{
	TransactionId creator = InvalidTransactionId;

	run("SELECT xmin FROM pg_catalog.pg_dolt_object WHERE objhash=$1 AND objkind='database'", 1, VC_TYPES(BYTEAOID), VC_ARGS(PointerGetDatum(database)), NULL, true);
	if (!SPI_processed)
		elog(ERROR, "canonical catalog root disappeared");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		TransactionId xid = DatumGetTransactionId(cell(i, 1));

		if (!TransactionIdIsCurrentTransactionId(xid))
			return InvalidTransactionId;
		creator = xid;
	}
	return creator;
}

/*
 * The catalog view of schema |database|: as a branch head's working schema
 * when |branch| is given, else as a read-only revision. The two differ only
 * in object identity, so one schema can be cached under both.
 */
static CatalogView *
provider_lookup(const char *branch, bytea *database)
{
	char	   *key = hex(database);
	DGProvider *entry;
	CatalogView *view;

	/*
	 * Visibility is required even on a cache hit: an aborted object must not
	 * survive through a backend cache.
	 */
	(void) metadata(database, "database");
	if (!identity_callback)
	{
		CacheRegisterRelcacheCallback(provider_identities_changed, (Datum) 0);
		identity_callback = true;
	}
	for (entry = providers; entry; entry = entry->next)
	{
		if (!strcmp(entry->database, key) && provider_current(entry, branch))
		{
			if (CatalogViewCreatorAborted(entry->view))
				CatalogViewSetCreator(entry->view, provider_creator(database));
			CatalogViewEnsureLoaded(entry->view);
			return entry->view;
		}
	}
	view = CatalogViewCreate(branch ? psprintf("%s@%s", key, branch) : key);
	if (branch)
		CatalogViewMarkBranchHead(view);

	/*
	 * Register before populating, so lookups made while building the view,
	 * including of its own durable objects, find it rather than recurse.
	 */
	entry = MemoryContextAlloc(TopMemoryContext, sizeof(*entry));
	entry->database = MemoryContextStrdup(TopMemoryContext, key);
	entry->branch = branch ? MemoryContextStrdup(TopMemoryContext, branch) : NULL;
	entry->view = view;
	entry->generation = identity_generation;
	entry->next = providers;
	providers = entry;
	PG_TRY();
	{
		CatalogViewSetCreator(view, provider_creator(database));
		provider_populate(view, database, branch);
	}
	PG_CATCH();
	{
		providers = entry->next;
		pfree(entry->database);
		if (entry->branch)
			pfree(entry->branch);
		pfree(entry);
		CatalogViewRelease(view);
		PG_RE_THROW();
	}
	PG_END_TRY();
	CatalogViewSetLoader(view, provider_reload, entry);
	return view;
}

static CatalogView *
provider_view(bytea *database)
{
	return provider_lookup(NULL, database);
}

/*
 * A view of schema |database| for questions about its definitions only, such
 * as whether a name exists. Reuses a branch head's view of the same schema so
 * one statement does not decode the schema twice.
 */
static CatalogView *
provider_schema_view(bytea *database)
{
	char	   *key = hex(database);

	for (DGProvider *entry = providers; entry; entry = entry->next)
		if (entry->branch && !strcmp(entry->database, key) && provider_current(entry, entry->branch))
			return provider_lookup(entry->branch, database);
	return provider_view(database);
}

/* The current schema of |branch|'s head, with its objects' durable OIDs. */
static CatalogView *
provider_branch_view(const char *branch, bytea *database)
{
	return provider_lookup(branch, database);
}

/*
 * The working schema of |branch| under a fresh catalog snapshot: the latest
 * committed schema and this transaction's own changes, as PostgreSQL reads
 * physical catalogs. NULL if the branch has no working set.
 */
static bytea *
catalog_working_schema(const char *branch)
{
	Relation	catalog = table_open(DoltRefRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	bytea	   *result = NULL;

	InvalidateCatalogSnapshot();
	ScanKeyInit(&key, Anum_pg_dolt_ref_refname, BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(psprintf("working/%s", branch)));
	scan = systable_beginscan(catalog, DoltRefKeyIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		bool		isnull;
		Datum		target = heap_getattr(tuple, Anum_pg_dolt_ref_reftarget, RelationGetDescr(catalog), &isnull);

		if (!isnull)
			result = DatumGetByteaPCopy(target);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return result;
}

/*
 * The committed content of durable relation |oid| in its branch's working
 * schema: what the relation holds until the branch writes a root of its own.
 */
bytea *
dg_catalog_durable_content(Oid oid)
{
	char	   *branch,
			   *name;
	char		kind;
	bytea	   *working,
			   *result = NULL;
	Oid			user;
	int			security,
				nest;

	if (!dg_identity_describe(oid, &branch, &kind, &name) || kind != DOLT_IDENTITY_RELATION)
		return NULL;
	working = catalog_working_schema(branch);
	if (!working)
		return NULL;
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PushActiveSnapshot(GetLatestSnapshot());
	PG_TRY();
	{
		nest = begin_api();
		run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index') AND CASE WHEN convert_from(t.objpayload,'UTF8') IS JSON OBJECT THEN convert_from(t.objpayload,'UTF8')::jsonb->>'name' END=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(working), CStringGetTextDatum(name)), NULL, true);
		if (SPI_processed == 1)
			result = child(DatumGetByteaPCopy(cell(0, 1)), 2);
		end_api(nest);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return result;
}

/*
 * The content of the table or index that leased OID |handle| names in its
 * revision: immutable, so the relation needs no storage root.
 */
bytea *
dg_catalog_revision_content(Oid handle)
{
	CatalogView *view = CatalogViewObjectContext(handle);
	const char *name = CatalogViewObjectIdentity(handle, RelationRelationId);
	DGProvider *entry;
	bytea	   *result = NULL;
	Oid			user;
	int			security,
				nest;

	for (entry = providers; entry && entry->view != view; entry = entry->next)
		;
	if (!entry || !name)
		return NULL;
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		run("SELECT t.objhash FROM pg_dolt_object_visible d CROSS JOIN LATERAL unnest(d.objchildren) ch JOIN pg_dolt_object_visible t ON t.objhash=ch WHERE d.objhash=$1 AND t.objkind IN ('table','index') AND CASE WHEN convert_from(t.objpayload,'UTF8') IS JSON OBJECT THEN convert_from(t.objpayload,'UTF8')::jsonb->>'name' END=$2", 2, VC_TYPES(BYTEAOID, TEXTOID), VC_ARGS(PointerGetDatum(unhex(entry->database)), CStringGetTextDatum(name)), NULL, true);
		if (SPI_processed == 1)
			result = child(DatumGetByteaPCopy(cell(0, 1)), 2);
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
	}
	PG_END_TRY();
	return result;
}

/*
 * The current view for durable OID |oid|: its branch's working schema under
 * the catalog snapshot. NULL if no branch defines the OID.
 */
CatalogView *
dg_catalog_resolve_durable(Oid oid)
{
	char	   *branch,
			   *name;
	char		kind;
	Oid			user;
	int			security,
				nest;
	bool		pushed = false;
	CatalogView *result = NULL;
	CatalogView *previous;

	if (!IsTransactionState() || !dg_identity_describe(oid, &branch, &kind, &name))
		return NULL;
	previous = CatalogViewSwitch(NULL);

	/* The schema is read as catalogs are: latest committed, not as of the
	 * transaction's snapshot. */
	PushActiveSnapshot(GetLatestSnapshot());
	pushed = true;
	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		bytea	   *working = catalog_working_schema(branch);

		nest = begin_api();
		if (working)
			result = provider_branch_view(branch, working);
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		if (pushed)
			PopActiveSnapshot();
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}

CatalogView *
dg_catalog_view(const char *database, const char *branch)
{
	Oid			user;
	int			security,
				nest;
	CatalogView *result;
	CatalogView *previous = CatalogViewSwitch(NULL);

	GetUserIdAndSecContext(&user, &security);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, security | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		nest = begin_api();
		result = provider_lookup(branch, unhex(database));
		end_api(nest);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, security);
		CatalogViewSwitch(previous);
	}
	PG_END_TRY();
	return result;
}
