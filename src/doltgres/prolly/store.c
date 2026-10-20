/* Immutable, hash-addressed memory chunk store; explicit endian-safe encoding. */
#ifdef PL_POSTGRES
#include "postgres.h"
#include "common/cryptohash.h"
#else
#include <openssl/evp.h>
#endif
#include "internal.h"
#include <limits.h>
#include <unistd.h>

static const uint8_t node_magic[8] = {'D', 'G', 'P', 'T', 'R', '0', '0', '1'};

uint32_t
pl_read32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
		((uint32_t) p[2] << 8) | p[3];
}

uint64_t
pl_read64(const uint8_t *p)
{
	return ((uint64_t) pl_read32(p) << 32) | pl_read32(p + 4);
}

void
pl_write32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

void
pl_write64(uint8_t *p, uint64_t v)
{
	pl_write32(p, (uint32_t) (v >> 32));
	pl_write32(p + 4, (uint32_t) v);
}

bool
pl_hash_bytes(PLSlice bytes, PLHash *out)
{
#ifdef PL_POSTGRES
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	bool		ok;

	if (!ctx)
		return false;
	ok = pg_cryptohash_init(ctx) == 0 &&
		pg_cryptohash_update(ctx, bytes.data, bytes.len) == 0 &&
		pg_cryptohash_final(ctx, out->bytes, PL_HASH_SIZE) == 0;
	pg_cryptohash_free(ctx);
	return ok;
#else
	unsigned int length = 0;

	return EVP_Digest(bytes.data, bytes.len, out->bytes, &length,
					  EVP_sha256(), NULL) == 1 && length == PL_HASH_SIZE;
#endif
}

int
pl_compare(PLSlice a, PLSlice b)
{
	size_t		common = a.len < b.len ? a.len : b.len;
	int			cmp = common ? memcmp(a.data, b.data, common) : 0;

	return cmp ? cmp : (a.len > b.len) - (a.len < b.len);
}

bool
pl_hash_equal(PLHash a, PLHash b)
{
	return memcmp(a.bytes, b.bytes, PL_HASH_SIZE) == 0;
}

const char *
pl_status_string(PLStatus status)
{
	static const char *const names[] = {"ok", "not found", "invalid input",
	"out of memory", "corrupt content", "merge conflict", "stopped", "I/O error", "store busy"};

	return (unsigned) status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

PLStore *
pl_store_create(void)
{
	PLStore    *store = calloc(1, sizeof(*store));

	if (!store)
		return NULL;
	store->fd = -1;
	store->capacity = 256;
	store->buckets = calloc(store->capacity, sizeof(*store->buckets));
	if (!store->buckets)
	{
		free(store);
		return NULL;
	}
	return store;
}

void
pl_store_free(PLStore *store)
{
	size_t		i;

	if (!store)
		return;
	for (i = 0; i < store->capacity; i++)
	{
		PLNode	   *node = store->buckets[i];

		while (node)
		{
			PLNode	   *next = node->next;

			free(node->data);
			free(node);
			node = next;
		}
	}
	if (store->fd >= 0)
		close(store->fd);
	free(store->buckets);
	free(store);
}

PLStats
pl_store_stats(const PLStore *store)
{
	return store->stats;
}

PLNode *
pl_find(PLStore *store, PLHash hash)
{
	PLNode	   *node;

	if (!store)
		return NULL;
	for (node = store->buckets[pl_read64(hash.bytes) % store->capacity];
		 node; node = node->next)
		if (pl_hash_equal(node->hash, hash))
			return node;
	return NULL;
}

PLSlice
pl_node_key(const PLNode *node, uint32_t index)
{
	const uint8_t *offsets = node->data + PL_HEADER;
	uint32_t	start = pl_read32(offsets + 4 * index);
	PLSlice		result = {node->data + PL_HEADER + 8 * (node->count + 1) + start,
	pl_read32(offsets + 4 * (index + 1)) - start};

	return result;
}

PLSlice
pl_node_value(const PLNode *node, uint32_t index)
{
	const uint8_t *offsets = node->data + PL_HEADER + 4 * (node->count + 1);
	uint32_t	start = pl_read32(offsets + 4 * index);
	PLSlice		result = {node->data + PL_HEADER + 8 * (node->count + 1) +
	node->key_bytes + start, pl_read32(offsets + 4 * (index + 1)) - start};

	return result;
}

uint64_t
pl_node_rows(const PLNode *node)
{
	uint64_t	rows = 0;
	uint32_t	i;

	if (node->level == 0)
		return node->count;
	for (i = 0; i < node->count; i++)
		rows += pl_read64(pl_node_value(node, i).data + PL_HASH_SIZE);
	return rows;
}

/* Decode metadata only after all offsets have been checked against buffer bounds. */
static PLStatus
parse_node(PLNode *node)
{
	uint32_t	i,
				which;
	size_t		directory;
	uint64_t	rows = 0;

	if (node->size < PL_HEADER || memcmp(node->data, node_magic, 8) != 0)
		return PL_CORRUPT;
	node->level = pl_read32(node->data + 8);
	node->count = pl_read32(node->data + 12);
	node->key_bytes = pl_read32(node->data + 16);
	node->value_bytes = pl_read32(node->data + 20);
	if (node->level >= PL_MAX_DEPTH || node->count > PL_MAX_ENTRIES ||
		(node->level && !node->count))
		return PL_CORRUPT;
	directory = PL_HEADER + 8 * ((size_t) node->count + 1);
	if (directory + (uint64_t) node->key_bytes + node->value_bytes != node->size)
		return PL_CORRUPT;
	for (which = 0; which < 2; which++)
	{
		const uint8_t *offsets = node->data + PL_HEADER + which * 4 * (node->count + 1);
		uint32_t	limit = which ? node->value_bytes : node->key_bytes;

		if (pl_read32(offsets) != 0 || pl_read32(offsets + 4 * node->count) != limit)
			return PL_CORRUPT;
		for (i = 0; i < node->count; i++)
		{
			uint32_t	start = pl_read32(offsets + 4 * i);
			uint32_t	end = pl_read32(offsets + 4 * (i + 1));

			if (end < start || end > limit || end - start > PL_MAX_ITEM ||
				(which && node->level && end - start != PL_CHILD_SIZE))
				return PL_CORRUPT;
		}
	}
	for (i = 0; i < node->count; i++)
	{
		if (i && pl_compare(pl_node_key(node, i - 1), pl_node_key(node, i)) >= 0)
			return PL_CORRUPT;
		if (node->level)
		{
			uint64_t	child_rows = pl_read64(pl_node_value(node, i).data + PL_HASH_SIZE);

			if (!child_rows || UINT64_MAX - rows < child_rows)
				return PL_CORRUPT;
			rows += child_rows;
		}
	}
	return PL_OK;
}

static PLStatus
store_buffer(PLStore *store, PLHash hash, const uint8_t *bytes, size_t length, PLNode **out)
{
	PLNode	   *node = pl_find(store, hash);
	size_t		bucket;

	if (!store)
		return PL_INVALID;
	if (store->failed)
		return PL_IO;
	if (node)
	{
		if (node->size != length || memcmp(node->data, bytes, length) != 0)
			return PL_CORRUPT;
		store->stats.reused++;
		*out = node;
		return PL_OK;
	}
	node = calloc(1, sizeof(*node));
	if (!node)
		return PL_NOMEM;
	node->data = malloc(length);
	if (!node->data)
	{
		free(node);
		return PL_NOMEM;
	}
	memcpy(node->data, bytes, length);
	node->size = length;
	node->hash = hash;
	if (parse_node(node) != PL_OK)
	{
		free(node->data);
		free(node);
		return PL_CORRUPT;
	}
	if (store->stats.chunks >= store->capacity * 2)
	{
		size_t		capacity = store->capacity * 2,
					i;
		PLNode	  **buckets = calloc(capacity, sizeof(*buckets));

		if (!buckets)
		{
			free(node->data);
			free(node);
			return PL_NOMEM;
		}
		for (i = 0; i < store->capacity; i++)
		{
			PLNode	   *current = store->buckets[i];

			while (current)
			{
				PLNode	   *next = current->next;

				bucket = pl_read64(current->hash.bytes) % capacity;
				current->next = buckets[bucket];
				buckets[bucket] = current;
				current = next;
			}
		}
		free(store->buckets);
		store->buckets = buckets;
		store->capacity = capacity;
	}
	if (store->fd >= 0 && !store->loading)
	{
		PLSlice		data = {bytes, length};
		PLStatus	status = pl_file_append(store, hash, data);

		if (status != PL_OK)
		{
			free(node->data);
			free(node);
			return status;
		}
	}
	bucket = pl_read64(hash.bytes) % store->capacity;
	node->next = store->buckets[bucket];
	store->buckets[bucket] = node;
	store->stats.chunks++;
	store->stats.bytes += length;
	*out = node;
	return PL_OK;
}

PLStatus
pl_store_export(PLStore *store, PLHash hash, PLSlice *bytes)
{
	PLNode	   *node = pl_find(store, hash);

	if (!bytes || !store)
		return PL_INVALID;
	if (!node)
		return PL_NOT_FOUND;
	bytes->data = node->data;
	bytes->len = node->size;
	return PL_OK;
}

PLStatus
pl_store_import(PLStore *store, PLHash hash, PLSlice bytes)
{
	PLHash		actual;
	PLNode	   *node;

	if (!store || !bytes.data || bytes.len > 2ULL * PL_MAX_ITEM * PL_MAX_ENTRIES + 2048)
		return PL_INVALID;
	if (!pl_hash_bytes(bytes, &actual) || !pl_hash_equal(hash, actual))
		return PL_CORRUPT;
	return store_buffer(store, hash, bytes.data, bytes.len, &node);
}

PLStatus
pl_save(PLStore *store, uint32_t level, const PLEntry *entries, uint32_t count, PLNode **out)
{
	size_t		key_bytes = 0,
				value_bytes = 0,
				length,
				start;
	uint32_t	i;
	uint8_t    *data;
	PLHash		hash;
	PLSlice		bytes;
	PLStatus	status;

	for (i = 0; i < count; i++)
	{
		key_bytes += entries[i].key.len;
		value_bytes += entries[i].value.len;
	}
	if (key_bytes > UINT32_MAX || value_bytes > UINT32_MAX)
		return PL_INVALID;
	length = PL_HEADER + 8 * ((size_t) count + 1) + key_bytes + value_bytes;
	data = calloc(1, length);
	if (!data)
		return PL_NOMEM;
	memcpy(data, node_magic, 8);
	pl_write32(data + 8, level);
	pl_write32(data + 12, count);
	pl_write32(data + 16, (uint32_t) key_bytes);
	pl_write32(data + 20, (uint32_t) value_bytes);
	start = PL_HEADER + 8 * (count + 1);
	key_bytes = value_bytes = 0;
	for (i = 0; i < count; i++)
	{
		if (entries[i].key.len)
			memcpy(data + start + key_bytes, entries[i].key.data, entries[i].key.len);
		key_bytes += entries[i].key.len;
		pl_write32(data + PL_HEADER + 4 * (i + 1), (uint32_t) key_bytes);
	}
	for (i = 0; i < count; i++)
	{
		if (entries[i].value.len)
			memcpy(data + start + key_bytes + value_bytes, entries[i].value.data, entries[i].value.len);
		value_bytes += entries[i].value.len;
		pl_write32(data + PL_HEADER + 4 * (count + 1) + 4 * (i + 1), (uint32_t) value_bytes);
	}
	bytes.data = data;
	bytes.len = length;
	if (!pl_hash_bytes(bytes, &hash))
	{
		free(data);
		return PL_CORRUPT;
	}
	store->stats.writes++;
	status = store_buffer(store, hash, data, length, out);
	free(data);
	return status;
}

bool
pl_boundary(uint32_t level, uint32_t count, size_t bytes, PLSlice key)
{
	/*
	 * Key-local, level-salted fingerprint; no dependence on insertion
	 * history.
	 */
	uint64_t	fingerprint = UINT64_C(14695981039346656037) ^ level;
	size_t		i;

	if (count < 2)
		return false;
	if (count >= PL_MAX_ENTRIES || bytes >= PL_MAX_BYTES)
		return true;
	if (bytes < PL_MIN_BYTES)
		return false;
	for (i = 0; i < key.len; i++)
		fingerprint = (fingerprint ^ key.data[i]) * UINT64_C(1099511628211);
	/* Avalanche the integer-key suffix before testing the boundary bits. */
	fingerprint ^= fingerprint >> 33;
	fingerprint *= UINT64_C(0xff51afd7ed558ccd);
	fingerprint ^= fingerprint >> 33;
	return (fingerprint & 31) == 0;
}
