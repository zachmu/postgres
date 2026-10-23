/* Canonical database schemas decoded into native, immutable catalog tuples.
 * This provider never copies a live user relation's definition. Only bootstrap
 * catalog definitions/types and global role identities come from local catalogs.
 */
typedef struct DGProvider
{
	char	   *database;
	CatalogView *view;
	struct DGProvider *next;
} DGProvider;
static DGProvider *providers;

static const Oid provider_catalogs[] = {
	NamespaceRelationId, RelationRelationId, AttributeRelationId,
	TypeRelationId, OperatorRelationId, RangeRelationId, DescriptionRelationId, DependRelationId, IndexRelationId, ConstraintRelationId, CastRelationId, StatisticRelationId, StatisticExtRelationId, StatisticExtDataRelationId, PublicationRelRelationId, PublicationNamespaceRelationId, InheritsRelationId, RewriteRelationId, TriggerRelationId, PolicyRelationId, AttrDefaultRelationId
};

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

static void
provider_constraints(DGProviderRows *rows, CatalogView *view, const char *definition)
{
	char	   *name = json_value(definition, "name");
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Oid			relid = CatalogViewLookupObject(view, RelationRelationId, name);
	Oid			ns = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
	TupleDesc	desc = provider_columns(definition);
	char	  **names,
			  **defs;
	int			n,
				i;

	run("SELECT v->>0,v->>1 FROM jsonb_array_elements(COALESCE($1::jsonb->'constraints','[]'::jsonb)) v", 1, VC_TYPES(TEXTOID), VC_ARGS(CStringGetTextDatum(definition)), NULL, true);
	n = SPI_processed;
	names = palloc_array(char *, Max(n, 1));
	defs = palloc_array(char *, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		names[i] = strcell(i, 1);
		defs[i] = strcell(i, 2);
	}
	for (i = 0; i < n; i++)
	{
		List	   *parsed = raw_parser(psprintf("ALTER TABLE placeholder ADD CONSTRAINT %s %s", quote_identifier(names[i]), defs[i]), RAW_PARSE_DEFAULT);
		Node	   *node = ((RawStmt *) linitial(parsed))->stmt;
		AlterTableStmt *alter;
		Constraint *constraint;
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
		form.conenforced = true;
		form.convalidated = !constraint->skip_validation;
		form.conislocal = true;
		form.connoinherit = constraint->is_no_inherit;
		form.confupdtype = ' ';
		form.confdeltype = ' ';
		form.confmatchtype = ' ';
		if (form.contype != CONSTRAINT_NOTNULL)
			form.conindid = CatalogViewLookupObject(view, RelationRelationId, quote_qualified_identifier(strVal(linitial(parts)), names[i]));
		keys = palloc_array(Datum, Max(list_length(constraint->keys), 1));
		foreach(lc, constraint->keys)
		{
			int			att = provider_attribute(desc, strVal(lfirst(lc)));

			keys[k++] = Int16GetDatum(att);
			provider_dependency(rows, ConstraintRelationId, form.oid, 0, RelationRelationId, relid, att, DEPENDENCY_AUTO);
		}
		tuple = provider_fixed(rows->relations[slot], &form, sizeof(form), Anum_pg_constraint_conperiod);
		tuple = provider_set(rows->relations[slot], tuple, Anum_pg_constraint_conkey, PointerGetDatum(construct_array(keys, k, INT2OID, 2, true, TYPALIGN_SHORT)));
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
	table = quote_qualified_identifier(stmt->relation->schemaname, stmt->relation->relname);
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

		if (con->conindid == form.indexrelid)
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

static CatalogView *
provider_view(bytea *database)
{
	char	   *key = hex(database);
	DGProvider *entry;
	CatalogView *view;
	DGProviderRows rows = {0};
	bytea	  **addresses;
	char	  **kinds,
			  **definitions;
	int			n,
				i,
				j;

	/*
	 * Validate visibility even on a cache hit; aborted objects cannot survive
	 * via a backend cache. The canonical root is immutable once visible.
	 */
	(void) metadata(database, "database");
	for (entry = providers; entry; entry = entry->next)
		if (!strcmp(entry->database, key))
			return entry->view;
	view = CatalogViewCreate(key);
	PG_TRY();
	{
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
			else if (!strcmp(kinds[i], "table") || !strcmp(kinds[i], "index"))
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
			if (strcmp(kinds[i], "namespace"))
				(void) CatalogViewBindObject(view, RelationRelationId, json_value(definitions[i], "name"), InvalidOid);
		for (i = 0; i < n; i++)
			if (strcmp(kinds[i], "namespace"))
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
				form.oid = CatalogViewBindObject(view, RelationRelationId, name, InvalidOid);
				form.relnamespace = CatalogViewLookupObject(view, NamespaceRelationId, strVal(linitial(parts)));
				namestrcpy(&form.relname, strVal(lsecond(parts)));
				form.relowner = get_role_oid(owner, false);
				form.reltype = index ? InvalidOid : CatalogViewBindObject(view, TypeRelationId, name, InvalidOid);
				form.relam = index ? DOLT_PROLLY_INDEX_AM_OID : DOLT_PROLLY_TABLE_AM_OID;
				form.relkind = index ? RELKIND_INDEX : RELKIND_RELATION;
				form.relpersistence = RELPERSISTENCE_PERMANENT;
				form.relnatts = desc->natts;
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

					attr->attrelid = form.oid;
					provider_add(&rows, AttributeRelationId, attr, ATTRIBUTE_FIXED_PART_SIZE, Anum_pg_attribute_attcollation);
					provider_dependency(&rows, RelationRelationId, form.oid, j + 1, TypeRelationId, attr->atttypid, 0, DEPENDENCY_NORMAL);
				}
				if (!index)
				{
					FormData_pg_type type = {0};
					int			attno;

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
			if (!strcmp(kinds[i], "table"))
				provider_constraints(&rows, view, definitions[i]);
		for (i = 0; i < n; i++)
			if (!strcmp(kinds[i], "index"))
				provider_index(&rows, view, definitions[i], n, definitions, kinds);
		for (i = 0; i < lengthof(provider_catalogs); i++)
		{
			CatalogViewAddRows(view, provider_catalogs[i], rows.rows[i]);
			table_close(rows.relations[i], AccessShareLock);
		}
		CatalogViewSeal(view);
	}
	PG_CATCH();
	{
		CatalogViewRelease(view);
		PG_RE_THROW();
	}
	PG_END_TRY();
	entry = MemoryContextAlloc(TopMemoryContext, sizeof(*entry));
	entry->database = MemoryContextStrdup(TopMemoryContext, key);
	entry->view = view;
	entry->next = providers;
	providers = entry;
	return view;
}

CatalogView *
dg_catalog_view(const char *database)
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
		result = provider_view(unhex(database));
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
