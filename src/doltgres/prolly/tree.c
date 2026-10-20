/* Ordered cursors and deterministic streaming construction. */
#include "internal.h"
#include <limits.h>

typedef struct PLBuilder
{
	PLStore *store;
	PLEntry pending[PL_MAX_ENTRIES];
	uint32_t count;
	size_t bytes;
	PLNode **nodes;
	size_t used;
	size_t capacity;
} PLBuilder;

static bool
valid_slice(PLSlice slice)
{
	return slice.len <= PL_MAX_ITEM && (slice.len == 0 || slice.data != NULL);
}

static uint32_t
lower_bound(const PLNode *node, PLSlice key)
{
	uint32_t lo = 0, hi = node->count;
	while (lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		if (pl_compare(pl_node_key(node, mid), key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static PLStatus
cursor_descend(PLCursor *cursor, PLNode *node, const PLSlice *lower)
{
	while (true)
	{
		uint32_t index = lower ? lower_bound(node, *lower) : 0;
		if (cursor->depth >= PL_MAX_DEPTH)
			return PL_CORRUPT;
		cursor->path[cursor->depth] = node;
		cursor->index[cursor->depth++] = index;
		if (index == node->count)
		{
			cursor->valid = false;
			return PL_OK;
		}
		if (!node->level)
		{
			cursor->valid = true;
			return PL_OK;
		}
		else
		{
			PLHash hash;
			PLNode *child;
			memcpy(hash.bytes, pl_node_value(node, index).data, PL_HASH_SIZE);
			child = pl_find(cursor->store, hash);
			if (!child || child->level + 1 != node->level)
				return PL_CORRUPT;
			node = child;
		}
	}
}

PLStatus
pl_cursor_init(PLCursor *cursor, const PLTree *tree, const PLSlice *lower)
{
	PLNode *root;
	memset(cursor, 0, sizeof(*cursor));
	if (!tree || !tree->store || (lower && !valid_slice(*lower)))
		return PL_INVALID;
	root = pl_find(tree->store, tree->root);
	if (!root)
		return PL_CORRUPT;
	cursor->store = tree->store;
	return cursor_descend(cursor, root, lower);
}

PLEntry
pl_cursor_entry(const PLCursor *cursor)
{
	uint32_t depth = cursor->depth - 1;
	PLEntry entry = {pl_node_key(cursor->path[depth], cursor->index[depth]),
		pl_node_value(cursor->path[depth], cursor->index[depth])};
	return entry;
}

PLStatus
pl_cursor_skip_leaf(PLCursor *cursor)
{
	if (!cursor->valid)
		return PL_OK;
	cursor->depth--;
	while (cursor->depth)
	{
		uint32_t depth = cursor->depth - 1;
		PLNode *parent = cursor->path[depth];
		if (++cursor->index[depth] < parent->count)
		{
			PLHash hash;
			PLNode *child;
			memcpy(hash.bytes, pl_node_value(parent, cursor->index[depth]).data, PL_HASH_SIZE);
			child = pl_find(cursor->store, hash);
			if (!child || child->level + 1 != parent->level)
				return PL_CORRUPT;
			return cursor_descend(cursor, child, NULL);
		}
		cursor->depth--;
	}
	cursor->valid = false;
	return PL_OK;
}

PLStatus
pl_cursor_next(PLCursor *cursor)
{
	uint32_t depth;
	if (!cursor->valid)
		return PL_OK;
	depth = cursor->depth - 1;
	if (++cursor->index[depth] < cursor->path[depth]->count)
		return PL_OK;
	return pl_cursor_skip_leaf(cursor);
}

static PLStatus
append_node(PLBuilder *builder, PLNode *node)
{
	if (builder->used == builder->capacity)
	{
		size_t capacity = builder->capacity ? builder->capacity * 2 : 16;
		PLNode **nodes;
		if (capacity < builder->capacity || capacity > SIZE_MAX / sizeof(*nodes))
			return PL_NOMEM;
		nodes = realloc(builder->nodes, capacity * sizeof(*nodes));
		if (!nodes)
			return PL_NOMEM;
		builder->nodes = nodes;
		builder->capacity = capacity;
	}
	builder->nodes[builder->used++] = node;
	return PL_OK;
}

static PLStatus
flush(PLBuilder *builder, uint32_t level)
{
	PLNode *node;
	PLStatus status = pl_save(builder->store, level, builder->pending, builder->count, &node);
	if (status != PL_OK)
		return status;
	status = append_node(builder, node);
	builder->count = 0;
	builder->bytes = 0;
	return status;
}

static PLStatus
add(PLBuilder *builder, PLEntry entry, uint32_t level)
{
	builder->pending[builder->count++] = entry;
	builder->bytes += 8 + entry.key.len + entry.value.len;
	if (pl_boundary(level, builder->count, builder->bytes, entry.key))
		return flush(builder, level);
	return PL_OK;
}

static PLStatus
finish(PLBuilder *builder, PLTree *out)
{
	uint32_t level = 0;
	PLStatus status = PL_OK;
	if (builder->count || !builder->used)
		status = flush(builder, 0);
	while (status == PL_OK && builder->used > 1)
	{
		PLNode **old = builder->nodes;
		size_t count = builder->used, i;
		uint8_t child_data[PL_MAX_ENTRIES][PL_CHILD_SIZE];
		builder->nodes = NULL;
		builder->used = builder->capacity = 0;
		if (++level >= PL_MAX_DEPTH)
		{
			free(old);
			return PL_INVALID;
		}
		for (i = 0; i < count && status == PL_OK; i++)
		{
			PLEntry entry;
			uint8_t *child = child_data[builder->count];
			memcpy(child, old[i]->hash.bytes, PL_HASH_SIZE);
			pl_write64(child + PL_HASH_SIZE, pl_node_rows(old[i]));
			entry.key = pl_node_key(old[i], old[i]->count - 1);
			entry.value.data = child; entry.value.len = PL_CHILD_SIZE;
			status = add(builder, entry, level);
		}
		if (status == PL_OK && builder->count)
			status = flush(builder, level);
		free(old);
	}
	if (status == PL_OK)
	{
		out->store = builder->store;
		out->root = builder->nodes[0]->hash;
	}
	return status;
}

PLStatus
pl_build(PLStore *store, const PLEntry *entries, size_t count, PLTree *out)
{
	PLBuilder builder = {0};
	PLStatus status = PL_OK;
	size_t i;
	if (!store || !out || (count && !entries))
		return PL_INVALID;
	for (i = 0; i < count; i++)
		if (!valid_slice(entries[i].key) || !valid_slice(entries[i].value) ||
			(i && pl_compare(entries[i - 1].key, entries[i].key) >= 0))
			return PL_INVALID;
	builder.store = store;
	for (i = 0; i < count && status == PL_OK; i++)
		status = add(&builder, entries[i], 0);
	if (status == PL_OK)
		status = finish(&builder, out);
	free(builder.nodes);
	return status;
}

PLTree
pl_branch(const PLTree *tree)
{
	return *tree;
}

PLStatus
pl_apply(const PLTree *tree, const PLEdit *edits, size_t count, PLTree *out)
{
	PLCursor cursor;
	PLBuilder builder = {0};
	PLStatus status;
	size_t i;
	if (!tree || !out || (count && !edits))
		return PL_INVALID;
	for (i = 0; i < count; i++)
		if (!valid_slice(edits[i].key) || (edits[i].value.present && !valid_slice(edits[i].value.bytes)) ||
			(i && pl_compare(edits[i - 1].key, edits[i].key) >= 0))
			return PL_INVALID;
	status = pl_cursor_init(&cursor, tree, NULL);
	if (status != PL_OK)
		return status;
	if (!count)
	{
		*out = *tree;
		return PL_OK;
	}
	builder.store = tree->store;
	i = 0;
	while (status == PL_OK && (cursor.valid || i < count))
	{
		PLEntry entry = {{NULL, 0}, {NULL, 0}};
		int comparison;
		if (cursor.valid)
		{
			PLNode *leaf = cursor.path[cursor.depth - 1];
			PLSlice last = pl_node_key(leaf, leaf->count - 1);
			/* Copy whole untouched chunks once chunk boundaries resynchronize. */
			if (builder.count == 0 && cursor.index[cursor.depth - 1] == 0 &&
				(i == count || pl_compare(edits[i].key, last) > 0) &&
				(i == count || pl_boundary(0, leaf->count, leaf->size - PL_HEADER - 8, last)))
			{
				status = append_node(&builder, leaf);
				if (status == PL_OK)
					status = pl_cursor_skip_leaf(&cursor);
				builder.store->stats.skipped++;
				continue;
			}
			entry = pl_cursor_entry(&cursor);
		}
		comparison = !cursor.valid ? 1 : i == count ? -1 : pl_compare(entry.key, edits[i].key);
		if (comparison < 0)
		{
			status = add(&builder, entry, 0);
			if (status == PL_OK)
				status = pl_cursor_next(&cursor);
		}
		else
		{
			if (edits[i].value.present)
			{
				entry.key = edits[i].key; entry.value = edits[i].value.bytes;
				status = add(&builder, entry, 0);
			}
			if (comparison == 0 && status == PL_OK)
				status = pl_cursor_next(&cursor);
			i++;
		}
	}
	if (status == PL_OK)
		status = finish(&builder, out);
	free(builder.nodes);
	return status;
}

PLStatus
pl_get(const PLTree *tree, PLSlice key, PLValue *out)
{
	PLCursor cursor;
	PLStatus status;
	if (!out)
		return PL_INVALID;
	status = pl_cursor_init(&cursor, tree, &key);
	if (status != PL_OK)
		return status;
	if (cursor.valid && pl_compare(pl_cursor_entry(&cursor).key, key) == 0)
	{
		out->present = true;
		out->bytes = pl_cursor_entry(&cursor).value;
	}
	else
	{
		out->present = false;
		out->bytes = (PLSlice) {NULL, 0};
	}
	return PL_OK;
}

PLStatus
pl_scan(const PLTree *tree, const PLSlice *lower, const PLSlice *upper,
		PLScanFn callback, void *arg)
{
	PLCursor cursor;
	PLStatus status;
	if (!callback || (upper && !valid_slice(*upper)))
		return PL_INVALID;
	status = pl_cursor_init(&cursor, tree, lower);
	while (status == PL_OK && cursor.valid)
	{
		PLEntry entry = pl_cursor_entry(&cursor);
		if (upper && pl_compare(entry.key, *upper) >= 0)
			break;
		status = callback(arg, entry.key, entry.value);
		if (status == PL_OK)
			status = pl_cursor_next(&cursor);
	}
	return status;
}

static PLStatus
validate_node(PLStore *store, PLNode *node, const PLSlice *lower, uint64_t *rows)
{
	uint32_t i;
	*rows = 0;
	if (lower && (!node->count || pl_compare(pl_node_key(node, 0), *lower) <= 0))
		return PL_CORRUPT;
	if (!node->level)
	{
		*rows = node->count;
		return PL_OK;
	}
	for (i = 0; i < node->count; i++)
	{
		PLSlice value = pl_node_value(node, i), upper = pl_node_key(node, i);
		PLHash hash;
		PLNode *child;
		uint64_t child_rows;
		PLStatus status;
		PLSlice previous;
		const PLSlice *child_lower = lower;
		if (i)
		{
			previous = pl_node_key(node, i - 1);
			child_lower = &previous;
		}
		memcpy(hash.bytes, value.data, PL_HASH_SIZE);
		child = pl_find(store, hash);
		if (!child || child->level + 1 != node->level || !child->count ||
			pl_compare(pl_node_key(child, child->count - 1), upper) != 0)
			return PL_CORRUPT;
		status = validate_node(store, child, child_lower, &child_rows);
		if (status != PL_OK || child_rows != pl_read64(value.data + PL_HASH_SIZE) ||
			UINT64_MAX - *rows < child_rows)
			return PL_CORRUPT;
		*rows += child_rows;
	}
	return PL_OK;
}

PLStatus
pl_validate(const PLTree *tree)
{
	PLNode *root;
	uint64_t rows;
	if (!tree || !tree->store)
		return PL_INVALID;
	root = pl_find(tree->store, tree->root);
	if (!root)
		return PL_CORRUPT;
	return validate_node(tree->store, root, NULL, &rows);
}

PLStatus
pl_open(PLStore *store, PLHash root, PLTree *out)
{
	PLTree candidate = {store, root};
	PLStatus status;
	if (!out)
		return PL_INVALID;
	status = pl_validate(&candidate);
	if (status == PL_OK)
		*out = candidate;
	return status;
}

uint64_t
pl_count(const PLTree *tree)
{
	PLNode *node = tree ? pl_find(tree->store, tree->root) : NULL;
	return node ? pl_node_rows(node) : 0;
}
