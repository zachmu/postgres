#include "internal.h"
#include <stdio.h>
#include <inttypes.h>

static unsigned checks;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)
#define OK(expression) CHECK((expression) == PL_OK)

static PLSlice
slice(const char *s)
{
	PLSlice result = {(const uint8_t *) s, strlen(s)};
	return result;
}

static void
test_tuples(void)
{
	PLValue fields[] = {{true, {NULL, 0}}, {false, {NULL, 0}}, {true, {NULL, 0}}};
	uint8_t key[8], *data = NULL;
	size_t size, i;
	PLValue value;
	PLSlice tuple;
	pl_key_int64(-123, key);
	fields[0].bytes = (PLSlice) {key, 8};
	OK(pl_tuple_pack(fields, 3, &data, &size));
	tuple = (PLSlice) {data, size};
	OK(pl_tuple_get(tuple, 0, &value));
	CHECK(value.present && pl_compare(value.bytes, fields[0].bytes) == 0);
	OK(pl_tuple_get(tuple, 1, &value)); CHECK(!value.present);
	OK(pl_tuple_get(tuple, 2, &value)); CHECK(value.present && !value.bytes.len);
	for (i = 0; i < size; i++)
		CHECK(pl_tuple_get((PLSlice) {data, i}, 0, &value) == PL_CORRUPT);
	CHECK(pl_tuple_get(tuple, 3, &value) == PL_NOT_FOUND);
	data[12] |= 128;
	CHECK(pl_tuple_get(tuple, 0, &value) == PL_CORRUPT);
	free(data);
}

static void
test_tree(void)
{
	PLStore *store = pl_store_create();
	PLTree tree, branch, fresh;
	PLEntry *entries = calloc(5000, sizeof(*entries));
	uint8_t (*keys)[8] = calloc(5000, 8);
	PLValue value;
	PLEdit edit;
	PLStats before, after;
	size_t i;
	CHECK(store && entries && keys);
	for (i = 0; i < 5000; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		entries[i] = (PLEntry) {{keys[i], 8}, slice("encoded tuple bytes")};
	}
	OK(pl_build(store, entries, 5000, &tree));
	OK(pl_validate(&tree)); CHECK(pl_count(&tree) == 5000);
	branch = pl_branch(&tree);
	CHECK(pl_hash_equal(branch.root, tree.root));
	for (i = 0; i < 5000; i += 37)
	{
		OK(pl_get(&tree, entries[i].key, &value));
		CHECK(value.present && pl_compare(value.bytes, entries[i].value) == 0);
	}
	edit = (PLEdit) {entries[2500].key, {true, slice("changed")}};
	before = pl_store_stats(store);
	OK(pl_apply(&branch, &edit, 1, &branch));
	after = pl_store_stats(store);
	CHECK(after.skipped > before.skipped + 20);
	CHECK(after.chunks - before.chunks < 20);
	OK(pl_get(&tree, edit.key, &value)); CHECK(pl_compare(value.bytes, entries[2500].value) == 0);
	entries[2500].value = slice("changed");
	OK(pl_build(store, entries, 5000, &fresh));
	CHECK(pl_hash_equal(fresh.root, branch.root));
	OK(pl_validate(&branch));
	printf("localized edit: %zu new chunks; %" PRIu64 " skipped leaves\n",
		after.chunks - before.chunks, after.skipped - before.skipped);
	free(keys); free(entries); pl_store_free(store);
}

int
main(void)
{
	test_tuples(); test_tree();
	printf("passed %u checks\n", checks);
	return 0;
}
