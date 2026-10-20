/* Shared-subtree-aware diff and row-level three-way merge. */
#include "internal.h"

static bool
same_value(PLValue a, PLValue b)
{
	return a.present == b.present && (!a.present || pl_compare(a.bytes, b.bytes) == 0);
}

static PLValue
present(PLSlice value)
{
	PLValue result = {true, value};
	return result;
}

static PLStatus
diff_cursors(const PLTree *before, const PLTree *after, PLDiffFn callback, void *arg)
{
	PLCursor a, b;
	PLStatus status = pl_cursor_init(&a, before, NULL);
	if (status == PL_OK)
		status = pl_cursor_init(&b, after, NULL);
	while (status == PL_OK && (a.valid || b.valid))
	{
		PLEntry x = {{NULL, 0}, {NULL, 0}}, y = x;
		PLValue absent = {false, {NULL, 0}};
		int comparison;
		if (a.valid && b.valid && a.index[a.depth - 1] == 0 && b.index[b.depth - 1] == 0 &&
			pl_hash_equal(a.path[a.depth - 1]->hash, b.path[b.depth - 1]->hash))
		{
			before->store->stats.skipped++;
			status = pl_cursor_skip_leaf(&a);
			if (status == PL_OK)
				status = pl_cursor_skip_leaf(&b);
			continue;
		}
		if (a.valid)
			x = pl_cursor_entry(&a);
		if (b.valid)
			y = pl_cursor_entry(&b);
		comparison = !a.valid ? 1 : !b.valid ? -1 : pl_compare(x.key, y.key);
		if (comparison < 0)
			status = callback(arg, x.key, present(x.value), absent);
		else if (comparison > 0)
			status = callback(arg, y.key, absent, present(y.value));
		else if (pl_compare(x.value, y.value) != 0)
			status = callback(arg, x.key, present(x.value), present(y.value));
		if (status == PL_OK && comparison <= 0)
			status = pl_cursor_next(&a);
		if (status == PL_OK && comparison >= 0)
			status = pl_cursor_next(&b);
	}
	return status;
}

static PLStatus
diff_nodes(const PLTree *before, const PLTree *after, PLDiffFn callback, void *arg,
		   unsigned depth)
{
	PLNode *a = pl_find(before->store, before->root);
	PLNode *b = pl_find(after->store, after->root);
	uint32_t i;
	if (!a || !b || depth >= PL_MAX_DEPTH)
		return PL_CORRUPT;
	if (pl_hash_equal(a->hash, b->hash))
	{
		before->store->stats.skipped++;
		return PL_OK;
	}
	if (a->level && a->level == b->level && a->count == b->count)
	{
		for (i = 0; i < a->count; i++)
			if (pl_compare(pl_node_key(a, i), pl_node_key(b, i)) != 0)
				break;
		if (i == a->count)
		{
			for (i = 0; i < a->count; i++)
			{
				PLTree left = {before->store, {{0}}}, right = {after->store, {{0}}};
				PLStatus status;
				memcpy(left.root.bytes, pl_node_value(a, i).data, PL_HASH_SIZE);
				memcpy(right.root.bytes, pl_node_value(b, i).data, PL_HASH_SIZE);
				status = diff_nodes(&left, &right, callback, arg, depth + 1);
				if (status != PL_OK)
					return status;
			}
			return PL_OK;
		}
	}
	/* A boundary shifted or height changed: reconcile ordered ranges by cursor. */
	return diff_cursors(before, after, callback, arg);
}

PLStatus
pl_diff(const PLTree *before, const PLTree *after, PLDiffFn callback, void *arg)
{
	if (!before || !after || !before->store || !after->store || !callback)
		return PL_INVALID;
	return diff_nodes(before, after, callback, arg, 0);
}

typedef struct PLChanges
{
	PLEdit *edits;
	size_t used;
	size_t capacity;
} PLChanges;

static PLStatus
record_change(void *arg, PLSlice key, PLValue before, PLValue after)
{
	PLChanges *changes = arg;
	(void) before;
	if (changes->used == changes->capacity)
	{
		size_t capacity = changes->capacity ? changes->capacity * 2 : 16;
		PLEdit *edits;
		if (capacity < changes->capacity || capacity > SIZE_MAX / sizeof(*edits))
			return PL_NOMEM;
		edits = realloc(changes->edits, capacity * sizeof(*edits));
		if (!edits)
			return PL_NOMEM;
		changes->edits = edits;
		changes->capacity = capacity;
	}
	changes->edits[changes->used++] = (PLEdit) {key, after};
	return PL_OK;
}

PLStatus
pl_merge(const PLTree *base, const PLTree *left, const PLTree *right,
		 PLResolveFn resolver, void *arg, PLTree *out)
{
	PLChanges a = {0}, b = {0}, result = {0};
	PLStatus status;
	uint8_t **owned = NULL;
	size_t i = 0, j = 0, allocated = 0;
	if (!base || !left || !right || !out || !base->store ||
		base->store != left->store || base->store != right->store)
		return PL_INVALID;
	if (!pl_find(base->store, base->root) || !pl_find(left->store, left->root) ||
		!pl_find(right->store, right->root))
		return PL_CORRUPT;
	if (pl_hash_equal(left->root, right->root) || pl_hash_equal(base->root, right->root))
	{
		*out = *left;
		return PL_OK;
	}
	if (pl_hash_equal(base->root, left->root))
	{
		*out = *right;
		return PL_OK;
	}
	status = pl_diff(base, left, record_change, &a);
	if (status == PL_OK)
		status = pl_diff(base, right, record_change, &b);
	if (status != PL_OK)
		goto done;
	owned = calloc(a.used + b.used, sizeof(*owned));
	if (!owned)
	{
		status = PL_NOMEM;
		goto done;
	}
	while (status == PL_OK && (i < a.used || j < b.used))
	{
		PLValue absent = {false, {NULL, 0}};
		int comparison = i == a.used ? 1 : j == b.used ? -1 :
			pl_compare(a.edits[i].key, b.edits[j].key);
		PLEdit chosen;
		if (comparison < 0)
			chosen = a.edits[i++];
		else if (comparison > 0)
			chosen = b.edits[j++];
		else
		{
			PLValue av = a.edits[i].value, bv = b.edits[j].value;
			chosen = a.edits[i];
			if (!same_value(av, bv))
			{
				PLValue original, resolved = absent;
				if (!resolver)
				{
					status = PL_CONFLICT;
					break;
				}
				status = pl_get(base, chosen.key, &original);
				if (status == PL_OK)
					status = resolver(arg, chosen.key, original, av, bv, &resolved);
				if (status != PL_OK)
					break;
				if (resolved.present && (resolved.bytes.len > PL_MAX_ITEM ||
					(resolved.bytes.len && !resolved.bytes.data)))
				{
					status = PL_INVALID;
					break;
				}
				chosen.value = resolved;
				if (resolved.present && resolved.bytes.len)
				{
					uint8_t *copy = malloc(resolved.bytes.len);
					if (!copy)
					{
						status = PL_NOMEM;
						break;
					}
					memcpy(copy, resolved.bytes.data, resolved.bytes.len);
					owned[allocated++] = copy;
					chosen.value.bytes.data = copy;
				}
			}
			i++; j++;
		}
		status = record_change(&result, chosen.key, absent, chosen.value);
	}
	if (status == PL_OK)
		status = pl_apply(base, result.edits, result.used, out);
done:
	for (i = 0; i < allocated; i++)
		free(owned[i]);
	free(owned);
	free(a.edits); free(b.edits); free(result.edits);
	return status;
}
