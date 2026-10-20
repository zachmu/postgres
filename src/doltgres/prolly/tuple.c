/* PostgreSQL-inspired schema-driven attribute storage, without heap MVCC fields. */
#include "internal.h"

static const uint8_t tuple_magic[8] = {'D','G','T','U','P','0','0','1'};

void
pl_key_int64(int64_t value, uint8_t out[8])
{
	pl_write64(out, ((uint64_t) value) ^ (UINT64_C(1) << 63));
}

PLStatus
pl_tuple_pack(const PLValue *fields, size_t count, uint8_t **buffer, size_t *length)
{
	size_t bitmap, header, bytes = 0, i, offset = 0;
	uint8_t *data;
	if (!buffer || !length || count > 1600 || (count && !fields))
		return PL_INVALID;
	bitmap = (count + 7) / 8;
	header = 12 + bitmap + 4 * (count + 1);
	for (i = 0; i < count; i++)
	{
		if (fields[i].present)
		{
			if (fields[i].bytes.len > PL_MAX_ITEM ||
				(fields[i].bytes.len && !fields[i].bytes.data))
				return PL_INVALID;
			bytes += fields[i].bytes.len;
			if (bytes + header > PL_MAX_ITEM)
				return PL_INVALID;
		}
	}
	data = calloc(1, header + bytes);
	if (!data)
		return PL_NOMEM;
	memcpy(data, tuple_magic, 8);
	pl_write32(data + 8, (uint32_t) count);
	for (i = 0; i < count; i++)
	{
		if (fields[i].present)
		{
			data[12 + i / 8] |= (uint8_t) (1 << (i % 8));
			if (fields[i].bytes.len)
				memcpy(data + header + offset, fields[i].bytes.data, fields[i].bytes.len);
			offset += fields[i].bytes.len;
		}
		pl_write32(data + 12 + bitmap + 4 * (i + 1), (uint32_t) offset);
	}
	*buffer = data;
	*length = header + bytes;
	return PL_OK;
}

PLStatus
pl_tuple_get(PLSlice tuple, size_t attribute, PLValue *out)
{
	size_t count, bitmap, header, i;
	const uint8_t *offsets;
	PLValue result;
	if (!out || !tuple.data || tuple.len < 12 || tuple.len > PL_MAX_ITEM ||
		memcmp(tuple.data, tuple_magic, 8) != 0)
		return PL_CORRUPT;
	count = pl_read32(tuple.data + 8);
	if (count > 1600)
		return PL_CORRUPT;
	bitmap = (count + 7) / 8;
	header = 12 + bitmap + 4 * (count + 1);
	if (header > tuple.len)
		return PL_CORRUPT;
	offsets = tuple.data + 12 + bitmap;
	if (pl_read32(offsets) != 0 || pl_read32(offsets + 4 * count) != tuple.len - header)
		return PL_CORRUPT;
	if (count % 8 && (tuple.data[12 + bitmap - 1] >> (count % 8)))
		return PL_CORRUPT;
	for (i = 0; i < count; i++)
	{
		uint32_t start = pl_read32(offsets + 4 * i), end = pl_read32(offsets + 4 * (i + 1));
		bool present = (tuple.data[12 + i / 8] & (1 << (i % 8))) != 0;
		if (end < start || end > tuple.len - header || (!present && end != start))
			return PL_CORRUPT;
	}
	if (attribute >= count)
		return PL_NOT_FOUND;
	result.present = (tuple.data[12 + attribute / 8] & (1 << (attribute % 8))) != 0;
	result.bytes.data = tuple.data + header + pl_read32(offsets + 4 * attribute);
	result.bytes.len = pl_read32(offsets + 4 * (attribute + 1)) - pl_read32(offsets + 4 * attribute);
	*out = result;
	return PL_OK;
}
