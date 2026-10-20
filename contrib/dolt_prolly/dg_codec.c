/*
 * Schema-driven storage serialization. SQL comparison is the index AM's job.
 * Preserve the original six encodings; new binary fields have a DGB1 envelope.
 * Arrays/composites recurse so nested TOAST pointers and padding never persist.
 */
#include "dg.h"
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/xml.h"

static PLSlice encode_value(Oid type, Datum value);
static Datum decode_value(Oid type, PLSlice bytes);

static PLSlice
pack_fields(PLValue *fields, int count)
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

				/*
				 * These senders omit the in-memory range type OID. Their
				 * bound types are known portable scalars. User-defined range
				 * senders may delegate to array_send/record_send, which embed
				 * type OIDs.
				 */
			case INT4RANGEOID:
			case INT8RANGEOID:
			case NUMRANGEOID:
			case TSRANGEOID:
			case TSTZRANGEOID:
			case DATERANGEOID:
			case INT4MULTIRANGEOID:
			case INT8MULTIRANGEOID:
			case NUMMULTIRANGEOID:
			case TSMULTIRANGEOID:
			case TSTZMULTIRANGEOID:
			case DATEMULTIRANGEOID:
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
			dg_unsupported("dropped columns");
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
	int			i;

	if (bytes.len < 12 || pl_read32(bytes.data + 8) != (uint32) desc->natts)
		dg_unsupported("schema changes to stored tuples");
	for (i = 0; i < desc->natts; i++)
	{
		PLValue		field;

		dg_check(pl_tuple_get(bytes, i, &field));
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

void
dg_decode(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot)
{
	HeapTuple	tuple;

	dg_validate_descriptor(RelationGetDescr(rel));
	tuple = decode_fields(RelationGetDescr(rel), bytes);

	tuple->t_self = *tid;
	tuple->t_tableOid = RelationGetRelid(rel);
	ExecForceStoreHeapTuple(tuple, slot, true);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
}
