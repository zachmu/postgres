/*
 * Schema-driven storage serialization. SQL comparison is the index AM's job.
 * Preserve the original six encodings; new binary fields have a DGB1 envelope.
 * Arrays/composites recurse so nested TOAST pointers and padding never persist.
 */
#include "dg.h"
#include "access/htup_details.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_type_d.h"
#include "common/int128.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "utils/inet.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "utils/typcache.h"
#include "utils/xml.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "common/hashfn.h"

static PLSlice encode_value(Oid type, Datum value);
static Datum decode_value(Oid type, PLSlice bytes);

static PLSlice
pack_fields(PLValue * fields, int count)
{
	uint8	   *packed;
	size_t		length;
	PLSlice		result;

	dg_check(pl_tuple_pack(fields, count, &packed, &length));
	result.data = palloc(length);
	result.len = length;
	memcpy((void *) result.data, packed, length);
	free(packed);
	return result;
}

/*
 * Validate schema, not just non-NULL datums. The recursive codecs strip array
 * element/composite type OIDs; arbitrary binary senders need not do so. Only
 * audited built-in binary formats and enum labels may reach that fallback.
 * Do not cache this check: ALTER TYPE can change a nested composite descriptor.
 */
static void
check_type_recursive(Oid type, List *ancestors)
{
	Oid			element;
	char		kind;

	check_stack_depth();
	type = getBaseType(type);
	if (list_member_oid(ancestors, type))
		return;
	ancestors = lappend_oid(list_copy(ancestors), type);
	element = get_element_type(type);
	kind = get_typtype(type);
	if (OidIsValid(element))
		check_type_recursive(element, ancestors);
	else if (kind == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
		int			i;

		for (i = 0; i < desc->natts; i++)
			if (!TupleDescAttr(desc, i)->attisdropped)
				check_type_recursive(TupleDescAttr(desc, i)->atttypid, ancestors);
		ReleaseTupleDesc(desc);
	}

	/*
	 * A range sends its flags and bounds, never its type OID, so it is as
	 * portable as its subtype; a subtype's own sender may embed OIDs.
	 */
	else if (kind == TYPTYPE_RANGE)
	{
		Oid			subtype = getBaseType(get_range_subtype(type));

		/* Nested in a range, arrays and records send their own type OIDs. */
		if (OidIsValid(get_element_type(subtype)) || get_typtype(subtype) == TYPTYPE_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prolly storage rejects type %s", format_type_be(type)),
					 errdetail("Only codecs verified to exclude non-portable identifiers are supported."),
					 errhint("OID and other local identifier translation is not implemented.")));
		check_type_recursive(subtype, ancestors);
	}
	else if (kind == TYPTYPE_MULTIRANGE)
		check_type_recursive(get_multirange_range(type), ancestors);
	else if (kind != TYPTYPE_ENUM)
	{
		switch (type)
		{
				/*
				 * Scalar senders below serialize values, never local object
				 * IDs.
				 */
			case BOOLOID:
			case BYTEAOID:
			case CHAROID:
			case NAMEOID:
			case INT2OID:
			case INT4OID:
			case INT8OID:
			case TEXTOID:
			case BPCHAROID:
			case VARCHAROID:
			case FLOAT4OID:
			case FLOAT8OID:
			case NUMERICOID:
			case CASHOID:
			case DATEOID:
			case TIMEOID:
			case TIMETZOID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
			case INTERVALOID:
			case BITOID:
			case VARBITOID:
			case UUIDOID:
			case INETOID:
			case CIDROID:
			case MACADDROID:
			case MACADDR8OID:
			case POINTOID:
			case LSEGOID:
			case PATHOID:
			case BOXOID:
			case POLYGONOID:
			case LINEOID:
			case CIRCLEOID:
			case JSONOID:
			case JSONBOID:
			case JSONPATHOID:
			case XMLOID:
			case TSVECTOROID:
			case TSQUERYOID:
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("prolly storage rejects type %s", format_type_be(type)),
						 errdetail("Only codecs verified to exclude non-portable identifiers are supported."),
						 errhint("OID and other local identifier translation is not implemented.")));
		}
	}
	list_free(ancestors);
}

static void
check_type(Oid type)
{
	check_type_recursive(type, NIL);
}

void
dg_validate_descriptor(TupleDesc desc)
{
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		check_type(TupleDescAttr(desc, i)->atttypid);
	}
}

/*
 * Binary I/O normally uses client_encoding; storage must use database encoding.
 * Restore backend conversion state even if a type function throws. xml_recv's
 * CONTENT mode accepts both XML documents and fragments, independent of the
 * reading session's xmloption. Domain constraints were checked by the executor
 * on assignment and must not be checked again when reading stored values.
 */
static bytea *
send_value(Oid type, Datum value)
{
	Oid			function;
	bool		varlena;
	int			encoding = pg_get_client_encoding();
	bytea	   *result = NULL;

	getTypeBinaryOutputInfo(type, &function, &varlena);
	PG_TRY();
	{
		if (SetClientEncoding(GetDatabaseEncoding()) < 0)
			elog(ERROR, "could not select storage encoding");
		result = OidSendFunctionCall(function, value);
	}
	PG_FINALLY();
	{
		if (SetClientEncoding(encoding) < 0)
			elog(FATAL, "could not restore client encoding");
	}
	PG_END_TRY();
	return result;
}

static Datum
receive_value(Oid type, PLSlice bytes)
{
	Oid			function,
				parameter;
	StringInfoData input;
	int			encoding = pg_get_client_encoding();
	int			saved_xmloption = xmloption;
	Datum		result = (Datum) 0;

	/* PostgreSQL 18's tsqueryrecv rejects the valid zero-item wire form. */
	if (type == TSQUERYOID && bytes.len == 4 && pl_read32(bytes.data) == 0)
	{
		TSQuery		query = palloc0(HDRSIZETQ);

		SET_VARSIZE(query, HDRSIZETQ);
		return PointerGetDatum(query);
	}
	getTypeBinaryInputInfo(type, &function, &parameter);
	initStringInfo(&input);
	appendBinaryStringInfo(&input, (const char *) bytes.data, bytes.len);
	PG_TRY();
	{
		if (SetClientEncoding(GetDatabaseEncoding()) < 0)
			elog(ERROR, "could not select storage encoding");
		xmloption = XMLOPTION_CONTENT;
		result = OidReceiveFunctionCall(function, &input, parameter, -1);
		if (input.cursor != input.len)
			elog(ERROR, "trailing bytes in prolly binary value");
	}
	PG_FINALLY();
	{
		xmloption = saved_xmloption;
		if (SetClientEncoding(encoding) < 0)
			elog(FATAL, "could not restore client encoding");
	}
	PG_END_TRY();
	pfree(input.data);
	return result;
}

static PLSlice
encode_array(Oid element, Datum value)
{
	ArrayType  *array = DatumGetArrayTypeP(value);
	Datum	   *values;
	bool	   *nulls;
	int			count,
				i,
				ndim = ARR_NDIM(array);
	int16		length;
	bool		byval;
	char		align;
	PLValue    *fields;
	uint8	   *shape = palloc(4 + 8 * ndim);
	PLSlice		result;

	get_typlenbyvalalign(element, &length, &byval, &align);
	deconstruct_array(array, element, length, byval, align, &values, &nulls, &count);
	fields = palloc0(sizeof(*fields) * (count + 1));
	pl_write32(shape, ndim);
	for (i = 0; i < ndim; i++)
	{
		pl_write32(shape + 4 + 8 * i, ARR_DIMS(array)[i]);
		pl_write32(shape + 8 + 8 * i, ARR_LBOUND(array)[i]);
	}
	fields[0].present = true;
	fields[0].bytes = (PLSlice)
	{
		shape, 4 + 8 * ndim
	};
	for (i = 0; i < count; i++)
	{
		fields[i + 1].present = !nulls[i];
		if (!nulls[i])
			fields[i + 1].bytes = encode_value(element, values[i]);
	}
	result = pack_fields(fields, count + 1);
	pfree(fields);
	pfree(values);
	pfree(nulls);
	return result;
}

static Datum
decode_array(Oid type, Oid element, PLSlice bytes)
{
	PLValue		shape,
				field;
	int			dims[MAXDIM],
				lbs[MAXDIM];
	int			ndim,
				count,
				i;
	Datum	   *values;
	bool	   *nulls;
	int16		length;
	bool		byval;
	char		align;
	ArrayType  *result;

	dg_check(pl_tuple_get(bytes, 0, &shape));
	if (!shape.present || shape.bytes.len < 4)
		elog(ERROR, "invalid prolly array shape");
	ndim = pl_read32(shape.bytes.data);
	if (ndim < 0 || ndim > MAXDIM || shape.bytes.len != 4 + 8 * ndim)
		elog(ERROR, "invalid prolly array dimensions");
	for (i = 0; i < ndim; i++)
	{
		dims[i] = pl_read32(shape.bytes.data + 4 + 8 * i);
		lbs[i] = (int32) pl_read32(shape.bytes.data + 8 + 8 * i);
	}
	count = ArrayGetNItems(ndim, dims);
	ArrayCheckBounds(ndim, dims, lbs);
	if (pl_read32(bytes.data + 8) != (uint32) count + 1)
		elog(ERROR, "invalid prolly array element count");
	/* Catalog vectors keep one zero-length dimension, unlike empty arrays. */
	if (count == 0 && type == INT2VECTOROID)
		return PointerGetDatum(buildint2vector(NULL, 0));
	values = palloc0(sizeof(*values) * Max(count, 1));
	nulls = palloc(sizeof(*nulls) * Max(count, 1));
	for (i = 0; i < count; i++)
	{
		dg_check(pl_tuple_get(bytes, i + 1, &field));
		nulls[i] = !field.present;
		if (field.present)
			values[i] = decode_value(element, field.bytes);
	}
	get_typlenbyvalalign(element, &length, &byval, &align);
	result = construct_md_array(values, nulls, ndim, dims, lbs, element, length, byval, align);
	for (i = 0; i < count; i++)
		if (!nulls[i] && !byval)
			pfree(DatumGetPointer(values[i]));
	pfree(values);
	pfree(nulls);
	return PointerGetDatum(result);
}

static PLSlice
encode_record(Oid type, Datum value)
{
	HeapTupleHeader header = DatumGetHeapTupleHeader(value);
	HeapTupleData tuple;
	TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
	Datum	   *values = palloc(sizeof(*values) * desc->natts);
	bool	   *nulls = palloc(sizeof(*nulls) * desc->natts);
	PLValue    *fields = palloc0(sizeof(*fields) * desc->natts);
	PLSlice		result;
	int			i;

	tuple.t_len = HeapTupleHeaderGetDatumLength(header);
	tuple.t_data = header;
	heap_deform_tuple(&tuple, desc, values, nulls);
	for (i = 0; i < desc->natts; i++)
	{
		fields[i].present = !nulls[i] && !TupleDescAttr(desc, i)->attisdropped;
		if (fields[i].present)
			fields[i].bytes = encode_value(TupleDescAttr(desc, i)->atttypid, values[i]);
	}
	result = pack_fields(fields, desc->natts);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);
	pfree(fields);
	return result;
}

static HeapTuple
decode_fields(TupleDesc desc, PLSlice bytes)
{
	Datum	   *values = palloc0(sizeof(*values) * desc->natts);
	bool	   *nulls = palloc(sizeof(*nulls) * desc->natts);
	HeapTuple	result;
	PLTupleView view;
	int			i;

	dg_check(pl_tuple_open(bytes, &view));
	if (view.count != (size_t) desc->natts)
		dg_unsupported("schema changes to stored tuples");
	for (i = 0; i < desc->natts; i++)
	{
		PLValue		field;

		dg_check(pl_tuple_field(&view, i, &field));
		nulls[i] = !field.present || TupleDescAttr(desc, i)->attisdropped;
		if (!nulls[i])
			values[i] = decode_value(TupleDescAttr(desc, i)->atttypid, field.bytes);
	}
	result = heap_form_tuple(desc, values, nulls);
	for (i = 0; i < desc->natts; i++)
		if (!nulls[i] && !TupleDescAttr(desc, i)->attbyval)
			pfree(DatumGetPointer(values[i]));
	pfree(values);
	pfree(nulls);
	return result;
}

static PLSlice
encode_value(Oid type, Datum value)
{
	PLSlice		result;
	Oid			element;
	uint8	   *data;
	bytea	   *binary;

	check_stack_depth();
	type = getBaseType(type);
	if (type == TEXTOID || type == BYTEAOID)
	{
		struct varlena *v = PG_DETOAST_DATUM_PACKED(value);

		return (PLSlice)
		{
			(uint8 *) VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v)
		};
	}
	if (type == BOOLOID || type == INT2OID || type == INT4OID || type == INT8OID)
	{
		int64		n = type == BOOLOID ? DatumGetBool(value) : type == INT2OID ? DatumGetInt16(value) : type == INT4OID ? DatumGetInt32(value) : DatumGetInt64(value);

		data = palloc(8);
		pl_key_int64(n, data);
		return (PLSlice)
		{
			data, 8
		};
	}

	element = get_element_type(type);
	if (OidIsValid(element))
		return encode_array(element, value);
	if (get_typtype(type) == TYPTYPE_COMPOSITE)
		return encode_record(type, value);
	check_type(type);
	binary = send_value(type, value);
	result.len = 4 + VARSIZE_ANY_EXHDR(binary);
	data = palloc(result.len);
	memcpy(data, "DGB1", 4);
	memcpy(data + 4, VARDATA_ANY(binary), result.len - 4);
	result.data = data;
	pfree(binary);
	return result;
}

static Datum
decode_value(Oid type, PLSlice bytes)
{
	Oid			element;

	check_stack_depth();
	type = getBaseType(type);
	if (type == TEXTOID || type == BYTEAOID)
	{
		struct varlena *v = palloc(VARHDRSZ + bytes.len);

		SET_VARSIZE(v, VARHDRSZ + bytes.len);
		memcpy(VARDATA(v), bytes.data, bytes.len);
		return PointerGetDatum(v);
	}
	if (type == BOOLOID || type == INT2OID || type == INT4OID || type == INT8OID)
	{
		int64		n;

		if (bytes.len != 8)
			elog(ERROR, "invalid prolly integer encoding");
		n = (int64) (pl_read64(bytes.data) ^ UINT64CONST(0x8000000000000000));
		return type == BOOLOID ? BoolGetDatum(n != 0) : type == INT2OID ? Int16GetDatum(n) : type == INT4OID ? Int32GetDatum(n) : Int64GetDatum(n);
	}

	element = get_element_type(type);
	if (OidIsValid(element))
		return decode_array(type, element, bytes);
	if (get_typtype(type) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);
		HeapTuple	tuple = decode_fields(desc, bytes);
		Datum		result = heap_copy_tuple_as_datum(tuple, desc);

		heap_freetuple(tuple);
		ReleaseTupleDesc(desc);
		return result;
	}
	if (bytes.len < 4 || memcmp(bytes.data, "DGB1", 4))
		elog(ERROR, "invalid prolly binary codec version");
	bytes.data += 4;
	bytes.len -= 4;
	return receive_value(type, bytes);
}

PLSlice
dg_encode(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext scratch = AllocSetContextCreate(CurrentMemoryContext, "prolly codec", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	PLValue    *fields;
	PLSlice		packed,
				result;
	int			i;

	dg_validate_descriptor(desc);
	slot_getallattrs(slot);
	old = MemoryContextSwitchTo(scratch);
	fields = palloc0(sizeof(*fields) * desc->natts);
	for (i = 0; i < desc->natts; i++)
	{
		fields[i].present = !slot->tts_isnull[i];
		if (fields[i].present)
			fields[i].bytes = encode_value(TupleDescAttr(desc, i)->atttypid, slot->tts_values[i]);
	}
	packed = pack_fields(fields, desc->natts);
	MemoryContextSwitchTo(old);
	result.data = palloc(packed.len);
	result.len = packed.len;
	memcpy((void *) result.data, packed.data, packed.len);
	MemoryContextDelete(scratch);
	return result;
}

/* Native ALTER scans may request the old descriptor for an expression rewrite
 * or the intermediate descriptor for constraint validation. Normalize only the
 * transient tuple shape; canonical publication chooses the final live columns. */
void
dg_decode_mutation(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	PLValue    *fields = palloc0_array(PLValue, Max(desc->natts, 1));
	uint8	   *packed;
	size_t		length;
	HeapTuple	tuple;
	uint32		count;
	PLSlice		normalized;

	if (bytes.len < 12)
		elog(ERROR, "invalid provisional source row");
	count = pl_read32(bytes.data + 8);
	for (int i = 0; i < desc->natts && i < count; i++)
		dg_check(pl_tuple_get(bytes, i, &fields[i]));
	dg_check(pl_tuple_pack(fields, desc->natts, &packed, &length));
	normalized.data = packed;
	normalized.len = length;
	tuple = dg_decode_descriptor(desc, normalized);
	free(packed);
	pfree(fields);
	tuple->t_self = *tid;
	tuple->t_tableOid = RelationGetRelid(rel);
	ExecForceStoreHeapTuple(tuple, slot, true);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
}

/* ---------------------------------------------------------------------------
 * Decode plans
 * ---------------------------------------------------------------------------
 *
 * Decoding a row used to validate the relation's descriptor and look every
 * column's type up again, per row. A decode plan does that once per relation:
 * it validates the descriptor, classifies each column, and resolves the
 * receive function of those that need one. Plans are keyed by relation OID
 * and dropped by relcache invalidation; a descriptor fingerprint guards the
 * private descriptors that share an OID across branches.
 */
typedef enum DecodeKind
{
	DECODE_INTEGER,				/* bool and the integer types */
	DECODE_BYTES,				/* text and bytea */
	DECODE_GENERAL,				/* arrays and composites: the recursive codec */
	DECODE_RECEIVE				/* a binary receive function */
} DecodeKind;

typedef struct DecodeColumn
{
	Oid			type;			/* the column's declared type */
	Oid			base;			/* its base type, which the codec uses */
	DecodeKind	kind;
	FmgrInfo	receive;
	Oid			ioparam;
	bool		dropped;
} DecodeColumn;

typedef struct DecodePlan
{
	Oid			relid;			/* hash key */
	TupleDesc	desc;
	uint32		fingerprint;
	int			natts;
	DecodeColumn *columns;
	bool		xml;			/* any column receives XML */
} DecodePlan;

static HTAB *decode_plans;
static bool decode_callback;

static void
decode_plans_changed(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	DecodePlan *plan;

	if (!decode_plans)
		return;
	hash_seq_init(&status, decode_plans);
	while ((plan = hash_seq_search(&status)) != NULL)
		if (!OidIsValid(relid) || plan->relid == relid)
		{
			pfree(plan->columns);
			hash_search(decode_plans, &plan->relid, HASH_REMOVE, NULL);
		}
}

/* The descriptor's shape, to notice a private descriptor replaced in place. */
static uint32
descriptor_fingerprint(TupleDesc desc)
{
	uint32		hash = (uint32) desc->natts;
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		hash = hash_combine(hash, (uint32) attr->atttypid);
		hash = hash_combine(hash, (uint32) attr->atttypmod);
		hash = hash_combine(hash, (uint32) attr->attisdropped);
	}
	return hash;
}

static void
decode_plan_build(DecodePlan *plan, TupleDesc desc)
{
	int			i;

	dg_validate_descriptor(desc);
	plan->desc = desc;
	plan->fingerprint = descriptor_fingerprint(desc);
	plan->natts = desc->natts;
	plan->columns = MemoryContextAllocZero(CacheMemoryContext, sizeof(DecodeColumn) * Max(desc->natts, 1));
	plan->xml = false;
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		DecodeColumn *column = &plan->columns[i];

		column->type = attr->atttypid;
		column->dropped = attr->attisdropped;
		if (column->dropped)
			continue;
		column->base = getBaseType(attr->atttypid);
		if (column->base == TEXTOID || column->base == BYTEAOID)
			column->kind = DECODE_BYTES;
		else if (column->base == BOOLOID || column->base == INT2OID || column->base == INT4OID || column->base == INT8OID)
			column->kind = DECODE_INTEGER;
		else if (OidIsValid(get_element_type(column->base)) || get_typtype(column->base) == TYPTYPE_COMPOSITE)
			column->kind = DECODE_GENERAL;
		else
		{
			Oid			function;

			column->kind = DECODE_RECEIVE;
			getTypeBinaryInputInfo(column->base, &function, &column->ioparam);
			fmgr_info_cxt(function, &column->receive, CacheMemoryContext);
			plan->xml |= column->base == XMLOID;
		}
	}
}

static DecodePlan *
decode_plan(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Oid			relid = RelationGetRelid(rel);
	DecodePlan *plan;
	bool		found;

	if (!decode_plans)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DecodePlan);
		ctl.hcxt = CacheMemoryContext;
		decode_plans = hash_create("prolly decode plans", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (!decode_callback)
	{
		CacheRegisterRelcacheCallback(decode_plans_changed, (Datum) 0);
		decode_callback = true;
	}
	plan = hash_search(decode_plans, &relid, HASH_ENTER, &found);
	if (found && plan->desc == desc && plan->natts == desc->natts && plan->fingerprint == descriptor_fingerprint(desc))
		return plan;
	if (found)
		pfree(plan->columns);
	plan->columns = NULL;
	PG_TRY();
	{
		decode_plan_build(plan, desc);
	}
	PG_CATCH();
	{
		hash_search(decode_plans, &relid, HASH_REMOVE, NULL);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return plan;
}

/* Decode one column's stored bytes with its planned receive function. */
static Datum
receive_planned(DecodeColumn *column, PLSlice bytes)
{
	StringInfoData input;
	Datum		result;

	if (bytes.len < 4 || memcmp(bytes.data, "DGB1", 4))
		elog(ERROR, "invalid prolly binary codec version");
	bytes.data += 4;
	bytes.len -= 4;

	/* PostgreSQL 18's tsqueryrecv rejects the valid zero-item wire form. */
	if (column->base == TSQUERYOID && bytes.len == 4 && pl_read32(bytes.data) == 0)
	{
		TSQuery		query = palloc0(HDRSIZETQ);

		SET_VARSIZE(query, HDRSIZETQ);
		return PointerGetDatum(query);
	}
	initStringInfo(&input);
	appendBinaryStringInfo(&input, (const char *) bytes.data, bytes.len);
	result = ReceiveFunctionCall(&column->receive, &input, column->ioparam, -1);
	if (input.cursor != input.len)
		elog(ERROR, "trailing bytes in prolly binary value");
	pfree(input.data);
	return result;
}

/*
 * Decode |bytes| into |slot| by |rel|'s plan: values land in the slot as a
 * virtual tuple, in the caller's memory, with no heap tuple in between.
 * Receive functions convert from the client encoding, so the row is decoded
 * under the database encoding when the two differ.
 */
void
dg_decode(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot)
{
	DecodePlan *plan = decode_plan(rel);
	PLTupleView view;
	int			encoding = pg_get_client_encoding();
	bool		switched = plan->xml || encoding != GetDatabaseEncoding();
	int			saved_xmloption = xmloption;
	int			i;

	if (!TTS_IS_VIRTUAL(slot))
	{
		HeapTuple	tuple = decode_fields(RelationGetDescr(rel), bytes);

		tuple->t_self = *tid;
		tuple->t_tableOid = RelationGetRelid(rel);
		ExecForceStoreHeapTuple(tuple, slot, true);
		slot->tts_tid = *tid;
		slot->tts_tableOid = RelationGetRelid(rel);
		return;
	}
	dg_check(pl_tuple_open(bytes, &view));
	if (view.count != (size_t) plan->natts)
		dg_unsupported("schema changes to stored tuples");
	ExecClearTuple(slot);
	if (switched)
	{
		if (SetClientEncoding(GetDatabaseEncoding()) < 0)
			elog(ERROR, "could not select storage encoding");
		xmloption = XMLOPTION_CONTENT;
	}
	PG_TRY();
	{
		for (i = 0; i < plan->natts; i++)
		{
			DecodeColumn *column = &plan->columns[i];
			PLValue		field;

			dg_check(pl_tuple_field(&view, i, &field));
			slot->tts_isnull[i] = !field.present || column->dropped;
			slot->tts_values[i] = (Datum) 0;
			if (slot->tts_isnull[i])
				continue;
			switch (column->kind)
			{
				case DECODE_BYTES:
					slot->tts_values[i] = PointerGetDatum(cstring_to_text_with_len((const char *) field.bytes.data, field.bytes.len));
					break;
				case DECODE_INTEGER:
					{
						int64		n;

						if (field.bytes.len != 8)
							elog(ERROR, "invalid prolly integer encoding");
						n = (int64) (pl_read64(field.bytes.data) ^ UINT64CONST(0x8000000000000000));
						slot->tts_values[i] = column->base == BOOLOID ? BoolGetDatum(n != 0) :
							column->base == INT2OID ? Int16GetDatum(n) :
							column->base == INT4OID ? Int32GetDatum(n) : Int64GetDatum(n);
						break;
					}
				case DECODE_GENERAL:
					slot->tts_values[i] = decode_value(column->type, field.bytes);
					break;
				case DECODE_RECEIVE:
					slot->tts_values[i] = receive_planned(column, field.bytes);
					break;
			}
		}
	}
	PG_FINALLY();
	{
		if (switched)
		{
			xmloption = saved_xmloption;
			if (SetClientEncoding(encoding) < 0)
				elog(FATAL, "could not restore client encoding");
		}
	}
	PG_END_TRY();
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
}

HeapTuple
dg_decode_descriptor(TupleDesc desc, PLSlice bytes)
{
	dg_validate_descriptor(desc);
	return decode_fields(desc, bytes);
}

/* The canonical bytes of one value of |type|, as stored in rows. */
PLSlice
dg_encode_value(Oid type, Datum value)
{
	return encode_value(type, value);
}

Datum
dg_decode_value(Oid type, PLSlice bytes)
{
	return decode_value(type, bytes);
}

/*
 * A conversion of stored values of type |from| to type |to| and typmod
 * |typmod|, by the assignment cast ALTER TABLE ... ALTER COLUMN TYPE would
 * apply, for a merge whose other side changed a column's type.
 */
DGConversion *
dg_conversion_begin(Oid from, Oid to, int32 typmod)
{
	DGConversion *conversion = palloc0(sizeof(DGConversion));
	Param	   *param = makeNode(Param);
	Node	   *expr;

	param->paramkind = PARAM_EXTERN;
	param->paramid = 1;
	param->paramtype = from;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(from);
	param->location = -1;
	expr = coerce_to_target_type(NULL, (Node *) param, from, to, typmod,
								 COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
	if (!expr)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot merge values of type %s into a column of type %s",
						format_type_be(from), format_type_with_typemod(to, typmod))));
	conversion->from = from;
	conversion->to = to;
	conversion->params = makeParamList(1);
	conversion->params->params[0].ptype = from;
	conversion->params->params[0].pflags = PARAM_FLAG_CONST;
	conversion->params->numParams = 1;
	conversion->state = ExecInitExprWithParams((Expr *) expression_planner((Expr *) expr), conversion->params);
	conversion->context = CreateStandaloneExprContext();
	conversion->context->ecxt_param_list_info = conversion->params;
	return conversion;
}

PLSlice
dg_conversion_apply(DGConversion *conversion, PLSlice bytes)
{
	bool		isnull;
	Datum		value;

	conversion->params->params[0].value = decode_value(conversion->from, bytes);
	conversion->params->params[0].isnull = false;
	value = ExecEvalExprSwitchContext(conversion->state, conversion->context, &isnull);
	if (isnull)
		elog(ERROR, "a merged value converted to NULL");
	return encode_value(conversion->to, value);
}

/*
 * A change to one user type that stored values must follow: an enum label
 * renamed, or a composite type's fields laid out again.
 */
typedef struct Recode
{
	Oid			target;
	const char *old;			/* enum label, and its new name */
	const char *renamed;
	int			count;			/* composite fields: the old field of each */
	const int  *mapping;		/* new one, or -1 for a new, absent field */
} Recode;

static PLSlice
recode_value(Oid type, PLSlice bytes, const Recode *recode)
{
	Oid			element;

	check_stack_depth();
	type = getBaseType(type);
	if (type == TEXTOID || type == BYTEAOID || type == BOOLOID || type == INT2OID ||
		type == INT4OID || type == INT8OID)
		return bytes;
	element = get_element_type(type);
	if (OidIsValid(element) || get_typtype(type) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = OidIsValid(element) ? NULL : lookup_rowtype_tupdesc(type, -1);
		uint32		count;
		PLValue    *fields;
		PLSlice		result;

		if (bytes.len < 12)
			elog(ERROR, "invalid prolly nested value");
		count = pl_read32(bytes.data + 8);
		fields = palloc0_array(PLValue, Max(count, 1));
		for (uint32 i = 0; i < count; i++)
			dg_check(pl_tuple_get(bytes, i, &fields[i]));
		if (type == recode->target && recode->mapping)
		{
			/* The old fields, in their new places; |desc| is the new layout. */
			PLValue    *moved = palloc0_array(PLValue, Max(recode->count, 1));

			for (int i = 0; i < recode->count; i++)
				if (recode->mapping[i] >= 0 && recode->mapping[i] < count)
					moved[i] = fields[recode->mapping[i]];
			fields = moved;
			count = recode->count;
		}
		for (uint32 i = 0; i < count; i++)
		{
			Oid			field;

			if (!fields[i].present)
				continue;
			/* An array's first field is its shape. */
			if (OidIsValid(element))
			{
				if (i == 0)
					continue;
				field = element;
			}
			else
				field = TupleDescAttr(desc, i)->atttypid;
			fields[i].bytes = recode_value(field, fields[i].bytes, recode);
		}
		result = pack_fields(fields, count);
		if (desc)
			ReleaseTupleDesc(desc);
		pfree(fields);
		return result;
	}
	if (type == recode->target && recode->old &&
		bytes.len == 4 + strlen(recode->old) && !memcmp(bytes.data + 4, recode->old, strlen(recode->old)))
	{
		uint8	   *data = palloc(4 + strlen(recode->renamed));

		memcpy(data, "DGB1", 4);
		memcpy(data + 4, recode->renamed, strlen(recode->renamed));
		return (PLSlice)
		{
			data, 4 + strlen(recode->renamed)
		};
	}
	return bytes;
}

/*
 * The canonical bytes of one value of |type| with every label |old| of enum
 * |target| replaced by |renamed|, as when the label is renamed. Stored enum
 * values are their labels; the structure of every type is unchanged.
 */
PLSlice
dg_recode_label(Oid type, PLSlice bytes, Oid target, const char *old, const char *renamed)
{
	Recode		recode = {
		.target = target,
		.old = old,
		.renamed = renamed,
	};

	return recode_value(type, bytes, &recode);
}

/*
 * The canonical bytes of one value of |type| with every value of composite
 * type |target| holding its fields as |target| now lays them out: new field i
 * is old field |mapping|[i], or absent if that is -1. Stored composite values
 * hold their fields by position, so a type's added or dropped attributes
 * change them.
 */
PLSlice
dg_recode_composite(Oid type, PLSlice bytes, Oid target, int count, const int *mapping)
{
	Recode		recode = {
		.target = target,
		.count = count,
		.mapping = mapping,
	};

	return recode_value(type, bytes, &recode);
}

/*
 * Index keys. A key column is its value's bytes in an order matching the
 * type's btree operator class, with values the class finds equal (such as
 * numeric 1.0 and 1.00, or float -0 and 0) encoded the same. For the types
 * whose row encoding already has these properties, it is the row encoding.
 */
static bool
key_is_row(Oid type)
{
	return type == BOOLOID || type == INT2OID || type == INT4OID || type == INT8OID ||
		type == TEXTOID || type == BYTEAOID;
}

static PLSlice
key_bytes(const void *data, size_t len)
{
	uint8	   *copy = palloc(Max(len, 1));

	memcpy(copy, data, len);
	return (PLSlice)
	{
		copy, len
	};
}

static PLSlice
key_int64(int64 n)
{
	uint8	   *data = palloc(8);

	pl_key_int64(n, data);
	return (PLSlice)
	{
		data, 8
	};
}

/* IEEE order, with -0 as 0 and every NaN above +Infinity, as float8 sorts. */
static PLSlice
key_float(double value)
{
	uint64		bits;
	uint8	   *data = palloc(8);

	if (value == 0)
		value = 0;
	if (isnan(value))
		bits = UINT64CONST(0x7FF8000000000000);
	else
		memcpy(&bits, &value, 8);
	bits = (bits & UINT64CONST(0x8000000000000000)) ? ~bits : bits | UINT64CONST(0x8000000000000000);
	pl_write64(data, bits);
	return (PLSlice)
	{
		data, 8
	};
}

/*
 * A numeric as a tag (-Infinity, negative, zero, positive, +Infinity, NaN),
 * then for a finite nonzero value its decimal exponent and significant
 * digits, trailing zeros removed. A negative value's exponent and digits are
 * complemented and end with 0xFF, so a longer magnitude sorts first.
 */
static PLSlice
key_numeric(Numeric value)
{
	char	   *text = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(value)));
	StringInfoData out;
	bool		negative = text[0] == '-';
	char	   *digits = palloc(strlen(text) + 1);
	int			n = 0,
				integer = -1,
				exponent;
	uint8		e[4];

	initStringInfo(&out);
	if (!strcmp(text, "NaN") || !strcmp(text, "Infinity") || !strcmp(text, "-Infinity"))
	{
		appendStringInfoChar(&out, text[0] == 'N' ? 6 : negative ? 1 : 5);
		return (PLSlice)
		{
			(uint8 *) out.data, out.len
		};
	}
	for (char *c = text + (negative ? 1 : 0); *c; c++)
	{
		if (*c == '.')
			integer = n;
		else
			digits[n++] = *c;
	}
	if (integer < 0)
		integer = n;
	exponent = integer;
	while (n > 0 && digits[0] == '0')
	{
		memmove(digits, digits + 1, --n);
		exponent--;
	}
	while (n > 0 && digits[n - 1] == '0')
		n--;
	if (n == 0)
	{
		appendStringInfoChar(&out, 3);
		return (PLSlice)
		{
			(uint8 *) out.data, out.len
		};
	}
	appendStringInfoChar(&out, negative ? 2 : 4);
	pl_write32(e, (uint32) exponent ^ 0x80000000);
	for (int i = 0; i < 4; i++)
		appendStringInfoChar(&out, negative ? ~e[i] : e[i]);
	for (int i = 0; i < n; i++)
		appendStringInfoChar(&out, negative ? ~digits[i] : digits[i]);
	if (negative)
		appendStringInfoChar(&out, (char) 0xFF);
	return (PLSlice)
	{
		(uint8 *) out.data, out.len
	};
}

/* An interval by the 128-bit span interval_cmp compares. */
static PLSlice
key_interval(Interval *interval)
{
	INT128		span = int64_to_int128(interval->time);
	uint8	   *data = palloc(16);
	int64		hi;
	uint64		lo;

	int128_add_int64_mul_int64(&span, interval->month * INT64CONST(30) + interval->day, USECS_PER_DAY);
#ifdef USE_NATIVE_INT128
	hi = (int64) (span >> 64);
	lo = (uint64) span;
#else
	hi = span.hi;
	lo = span.lo;
#endif
	pl_write64(data, (uint64) hi ^ UINT64CONST(0x8000000000000000));
	pl_write64(data + 8, lo);
	return (PLSlice)
	{
		data, 16
	};
}

/* Whether a prolly index can order keys of |type| as its operator class does. */
bool
dg_key_ordered(Oid type)
{
	type = getBaseType(type);
	switch (type)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case TEXTOID:
		case BYTEAOID:
		case VARCHAROID:
		case BPCHAROID:
		case CHAROID:
		case NAMEOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMEOID:
		case TIMETZOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case INTERVALOID:
		case UUIDOID:
		case CASHOID:
		case MACADDROID:
		case MACADDR8OID:
			return true;
		default:
			return get_typtype(type) == TYPTYPE_ENUM;
	}
}

/* The key bytes of |value| of |type|. */
PLSlice
dg_key_value(Oid type, Datum value)
{
	type = getBaseType(type);
	if (key_is_row(type))
		return encode_value(type, value);
	switch (type)
	{
		case VARCHAROID:
			{
				struct varlena *v = PG_DETOAST_DATUM_PACKED(value);

				return key_bytes(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
			}
		case BPCHAROID:
			{
				/* bpchar compares without its trailing spaces. */
				struct varlena *v = PG_DETOAST_DATUM_PACKED(value);
				size_t		len = VARSIZE_ANY_EXHDR(v);

				while (len > 0 && VARDATA_ANY(v)[len - 1] == ' ')
					len--;
				return key_bytes(VARDATA_ANY(v), len);
			}
		case NAMEOID:
			return key_bytes(NameStr(*DatumGetName(value)), strlen(NameStr(*DatumGetName(value))));
		case CHAROID:
			{
				uint8		c = (uint8) DatumGetChar(value);

				return key_bytes(&c, 1);
			}
		case FLOAT4OID:
			return key_float(DatumGetFloat4(value));
		case FLOAT8OID:
			return key_float(DatumGetFloat8(value));
		case NUMERICOID:
			return key_numeric(DatumGetNumeric(value));
		case DATEOID:
			return key_int64(DatumGetDateADT(value));
		case TIMEOID:
			return key_int64(DatumGetTimeADT(value));
		case TIMESTAMPOID:
			return key_int64(DatumGetTimestamp(value));
		case TIMESTAMPTZOID:
			return key_int64(DatumGetTimestampTz(value));
		case CASHOID:
			return key_int64(DatumGetCash(value));
		case TIMETZOID:
			{
				/* As timetz_cmp: UTC time, then zone. */
				TimeTzADT  *time = DatumGetTimeTzADTP(value);
				PLSlice		when = key_int64(time->time + time->zone * USECS_PER_SEC);
				PLSlice		zone = key_int64(time->zone);
				uint8	   *data = palloc(16);

				memcpy(data, when.data, 8);
				memcpy(data + 8, zone.data, 8);
				return (PLSlice)
				{
					data, 16
				};
			}
		case INTERVALOID:
			return key_interval(DatumGetIntervalP(value));
		case UUIDOID:
			return key_bytes(DatumGetUUIDP(value)->data, UUID_LEN);
		case MACADDROID:
			return key_bytes(DatumGetMacaddrP(value), sizeof(macaddr));
		case MACADDR8OID:
			return key_bytes(DatumGetMacaddr8P(value), sizeof(macaddr8));
	}
	if (get_typtype(type) == TYPTYPE_ENUM)
	{
		/* A label by its position in its enum. */
		HeapTuple	tuple = SearchSysCache1(ENUMOID, value);
		PLSlice		result;

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for enum label %u", DatumGetObjectId(value));
		result = key_float(((Form_pg_enum) GETSTRUCT(tuple))->enumsortorder);
		ReleaseSysCache(tuple);
		return result;
	}
	dg_unsupported("ordered index keys of this type");
	pg_unreachable();
}

/* The key bytes of a value stored in a row as |bytes|. */
PLSlice
dg_key_field(Oid type, PLSlice bytes)
{
	type = getBaseType(type);
	if (key_is_row(type))
		return bytes;
	return dg_key_value(type, decode_value(type, bytes));
}
