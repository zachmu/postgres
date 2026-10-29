/*-------------------------------------------------------------------------
 *
 * dg_schema_cache.c
 *	  Schema objects decoded once, by address.
 *
 * A schema object's address names immutable content, so what is decoded or
 * derived from it never needs invalidation, only eviction for space. Each
 * backend keeps the schemas it decoded, with the parse trees and column
 * descriptors derived from them, until its least recently used ones exceed
 * dolt_schema_cache_size. Serialized copies are shared between backends up to
 * dolt_shared_schema_cache_size, so a new session builds its catalog without
 * decoding schemas another session already decoded.
 *
 * Only what a definition alone determines is kept: built-in type, collation
 * and routine OIDs, never the OIDs of versioned objects, which differ between
 * branches and sessions, nor of roles, which may be dropped and recreated.
 * Callers fill those in when they build catalog rows from an entry.
 *
 * An entry stays valid until the transaction that obtained it ends: eviction
 * waits for then.
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"
#include "access/doltschema.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "lib/dshash.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "nodes/readfuncs.h"
#include "port/atomics.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/fmgrprotos.h"
#include "mb/pg_wchar.h"

int			dolt_schema_cache_size = 32 * 1024; /* kB */
int			dolt_shared_schema_cache_size = 64 * 1024;	/* kB */

/* Hashed and compared as bytes: padding must be zero. */
typedef struct SchemaKey
{
	uint8		address[PL_HASH_SIZE];
	char		kind[16];
	int32		encoding;		/* of the decoded text */
} SchemaKey;

typedef struct CachedSchema
{
	DGSchema	schema;			/* first, so a DGSchema is a CachedSchema */
	SchemaKey	key;
	MemoryContext memory;		/* holds the entry and all it points to */
	Size		size;
	bool		cached;			/* in the local cache, rather than transient */
	dlist_node	lru;
} CachedSchema;

typedef struct LocalSchema
{
	SchemaKey	key;
	CachedSchema *entry;
} LocalSchema;

/* Entries by definition text, for callers that hold only the text. */
typedef struct TextSchema
{
	const char *text;
	CachedSchema *entry;
} TextSchema;

static MemoryContext schema_memory;
static HTAB *local_schemas;
static HTAB *text_schemas;
static dlist_head schema_lru = DLIST_STATIC_INIT(schema_lru);
static Size local_bytes;

typedef struct SharedSchema
{
	SchemaKey	key;
	dsa_pointer data;
	Size		size;
	uint32		parts;
	uint64		used;			/* clock value at last use */
} SharedSchema;

typedef struct SharedSchemaTable
{
	int			tranche;
	dsa_handle	area;
	dshash_table_handle table;
	pg_atomic_uint64 bytes;
	pg_atomic_uint64 clock;
	pg_atomic_flag evicting;
} SharedSchemaTable;

static SharedSchemaTable *shared_control;
static dsa_area *shared_area;
static dshash_table *shared_schemas;

static const dshash_parameters shared_params = {
	sizeof(SchemaKey), sizeof(SharedSchema), dshash_memcmp, dshash_memhash, dshash_memcpy, 0
};

static uint32
text_hash(const void *key, Size keysize)
{
	const char *text = *(const char *const *) key;

	return hash_bytes((const unsigned char *) text, strlen(text));
}

static int
text_match(const void *a, const void *b, Size keysize)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* Evict least recently used entries above the configured size. */
static void
local_evict(void)
{
	Size		limit = (Size) dolt_schema_cache_size * 1024;

	while (local_bytes > limit && !dlist_is_empty(&schema_lru))
	{
		CachedSchema *entry = dlist_tail_element(CachedSchema, lru, &schema_lru);
		TextSchema *text = hash_search(text_schemas, &entry->schema.json, HASH_FIND, NULL);

		if (text && text->entry == entry)
			hash_search(text_schemas, &entry->schema.json, HASH_REMOVE, NULL);
		hash_search(local_schemas, &entry->key, HASH_REMOVE, NULL);
		dlist_delete(&entry->lru);
		local_bytes -= entry->size;
		MemoryContextDelete(entry->memory);
	}
}

static void
schema_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PREPARE ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
		local_evict();
}

static void
local_init(void)
{
	HASHCTL		ctl = {0};

	if (local_schemas)
		return;
	schema_memory = AllocSetContextCreate(TopMemoryContext, "decoded schemas", ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(SchemaKey);
	ctl.entrysize = sizeof(LocalSchema);
	ctl.hcxt = schema_memory;
	local_schemas = hash_create("decoded schemas", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.keysize = sizeof(const char *);
	ctl.entrysize = sizeof(TextSchema);
	ctl.hash = text_hash;
	ctl.match = text_match;
	text_schemas = hash_create("decoded schema texts", 256, &ctl, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
	RegisterXactCallback(schema_xact, NULL);
}

static void
shared_init(void *p)
{
	SharedSchemaTable *t = p;
	MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
	dshash_parameters params = shared_params;

	t->tranche = LWLockNewTrancheId();
	LWLockRegisterTranche(t->tranche, "dolt decoded schemas");
	shared_area = dsa_create(t->tranche);
	dsa_pin(shared_area);
	dsa_pin_mapping(shared_area);
	params.tranche_id = t->tranche;
	shared_schemas = dshash_create(shared_area, &params, NULL);
	t->area = dsa_get_handle(shared_area);
	t->table = dshash_get_hash_table_handle(shared_schemas);
	pg_atomic_init_u64(&t->bytes, 0);
	pg_atomic_init_u64(&t->clock, 0);
	pg_atomic_init_flag(&t->evicting);
	MemoryContextSwitchTo(previous);
}

/* Whether schemas are shared; attaches to them on first use. */
static bool
shared_attach(void)
{
	bool		found;

	if (dolt_shared_schema_cache_size == 0 || !IsUnderPostmaster)
		return false;
	if (shared_schemas)
		return true;
	shared_control = GetNamedDSMSegment("dolt decoded schemas", sizeof(SharedSchemaTable), shared_init, &found);
	if (!shared_schemas)
	{
		MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
		dshash_parameters params = shared_params;

		LWLockRegisterTranche(shared_control->tranche, "dolt decoded schemas");
		shared_area = dsa_attach(shared_control->area);
		dsa_pin_mapping(shared_area);
		params.tranche_id = shared_control->tranche;
		shared_schemas = dshash_attach(shared_area, &params, shared_control->table, NULL);
		MemoryContextSwitchTo(previous);
	}
	return true;
}

static void
put_bytes(StringInfo buf, const void *data, int32 len)
{
	appendBinaryStringInfo(buf, &len, sizeof(len));
	if (len > 0)
		appendBinaryStringInfo(buf, data, len);
}

/* A node as its text, with the terminator, so it can be read in place. */
static void
put_node(StringInfo buf, Node *node)
{
	char	   *text;

	if (!node)
	{
		put_bytes(buf, NULL, -1);
		return;
	}
	text = nodeToString(node);
	put_bytes(buf, text, strlen(text) + 1);
	pfree(text);
}

static void
put_nodes(StringInfo buf, int count, Node **nodes)
{
	appendBinaryStringInfo(buf, &count, sizeof(count));
	for (int i = 0; i < count; i++)
		put_node(buf, nodes[i]);
}

static void
schema_serialize(DGSchema *schema, StringInfo buf)
{
	initStringInfo(buf);
	appendBinaryStringInfo(buf, &schema->parts, sizeof(schema->parts));
	put_bytes(buf, schema->json, strlen(schema->json) + 1);
	put_bytes(buf, schema->document, VARSIZE(schema->document));
	if (schema->parts & DG_SCHEMA_COLUMNS)
	{
		appendBinaryStringInfo(buf, &schema->columns->natts, sizeof(schema->columns->natts));
		for (int i = 0; i < schema->columns->natts; i++)
			appendBinaryStringInfo(buf, TupleDescAttr(schema->columns, i), sizeof(FormData_pg_attribute));
	}
	if (schema->parts & DG_SCHEMA_DEFAULTS)
		put_nodes(buf, schema->ndefaults, schema->defaults);
	if (schema->parts & DG_SCHEMA_CONSTRAINTS)
		put_nodes(buf, schema->nconstraints, schema->constraints);
	if (schema->parts & DG_SCHEMA_INDEX)
		put_node(buf, schema->index);
	if (schema->parts & DG_SCHEMA_OPCLASSES)
	{
		appendBinaryStringInfo(buf, &schema->nopclasses, sizeof(schema->nopclasses));
		appendBinaryStringInfo(buf, schema->opclass_types, sizeof(Oid) * schema->nopclasses);
		appendBinaryStringInfo(buf, schema->opclasses, sizeof(Oid) * schema->nopclasses);
	}
}

typedef struct Reader
{
	char	   *data;
	Size		len;
	Size		offset;
} Reader;

static void
get(Reader *reader, void *dest, Size len)
{
	if (reader->offset + len > reader->len)
		elog(ERROR, "truncated shared schema entry");
	memcpy(dest, reader->data + reader->offset, len);
	reader->offset += len;
}

/* The next value put with put_bytes, in place, or NULL for none. */
static char *
get_bytes(Reader *reader, int32 *len)
{
	char	   *result;

	get(reader, len, sizeof(*len));
	if (*len < 0)
		return NULL;
	if (reader->offset + *len > reader->len)
		elog(ERROR, "truncated shared schema entry");
	result = reader->data + reader->offset;
	reader->offset += *len;
	return result;
}

static Node *
get_node(Reader *reader)
{
	int32		len;
	char	   *text = get_bytes(reader, &len);

	return text ? stringToNode(text) : NULL;
}

static Node **
get_nodes(Reader *reader, int *count)
{
	Node	  **nodes;

	get(reader, count, sizeof(*count));
	nodes = palloc0_array(Node *, Max(*count, 1));
	for (int i = 0; i < *count; i++)
		nodes[i] = get_node(reader);
	return nodes;
}

static CachedSchema *
schema_create(const SchemaKey *key, MemoryContext parent)
{
	MemoryContext memory = AllocSetContextCreate(parent, "decoded schema", ALLOCSET_SMALL_SIZES);
	CachedSchema *entry = MemoryContextAllocZero(memory, sizeof(CachedSchema));

	entry->key = *key;
	entry->memory = memory;
	return entry;
}

/* An entry from its serialized form, in |memory|'s child. */
static CachedSchema *
schema_deserialize(const SchemaKey *key, char *data, Size len, MemoryContext parent)
{
	CachedSchema *entry = schema_create(key, parent);
	DGSchema   *schema = &entry->schema;
	MemoryContext previous = MemoryContextSwitchTo(entry->memory);
	Reader		reader = {.data = data,.len = len};
	int32		size;
	char	   *bytes;

	get(&reader, &schema->parts, sizeof(schema->parts));
	schema->json = pstrdup(get_bytes(&reader, &size));
	bytes = get_bytes(&reader, &size);
	schema->document = palloc(size);
	memcpy(schema->document, bytes, size);
	if (schema->parts & DG_SCHEMA_COLUMNS)
	{
		int			natts;

		get(&reader, &natts, sizeof(natts));
		schema->columns = CreateTemplateTupleDesc(natts);
		for (int i = 0; i < natts; i++)
		{
			get(&reader, TupleDescAttr(schema->columns, i), sizeof(FormData_pg_attribute));
			populate_compact_attribute(schema->columns, i);
		}
	}
	if (schema->parts & DG_SCHEMA_DEFAULTS)
		schema->defaults = get_nodes(&reader, &schema->ndefaults);
	if (schema->parts & DG_SCHEMA_CONSTRAINTS)
		schema->constraints = get_nodes(&reader, &schema->nconstraints);
	if (schema->parts & DG_SCHEMA_INDEX)
		schema->index = get_node(&reader);
	if (schema->parts & DG_SCHEMA_OPCLASSES)
	{
		get(&reader, &schema->nopclasses, sizeof(schema->nopclasses));
		schema->opclass_types = palloc_array(Oid, Max(schema->nopclasses, 1));
		schema->opclasses = palloc_array(Oid, Max(schema->nopclasses, 1));
		get(&reader, schema->opclass_types, sizeof(Oid) * schema->nopclasses);
		get(&reader, schema->opclasses, sizeof(Oid) * schema->nopclasses);
	}
	if (reader.offset != reader.len)
		elog(ERROR, "malformed shared schema entry");
	MemoryContextSwitchTo(previous);
	return entry;
}

#ifdef USE_ASSERT_CHECKING
/* Serialization keeps every part, as the entry reading it expects. */
static void
schema_check_serialized(CachedSchema *entry, StringInfo buf)
{
	CachedSchema *copy = schema_deserialize(&entry->key, buf->data, buf->len, CurrentMemoryContext);
	DGSchema   *a = &entry->schema,
			   *b = &copy->schema;

	Assert(a->parts == b->parts);
	Assert(!strcmp(a->json, b->json));
	Assert(VARSIZE(a->document) == VARSIZE(b->document) && !memcmp(a->document, b->document, VARSIZE(a->document)));
	if (a->parts & DG_SCHEMA_COLUMNS)
		Assert(equalRowTypes(a->columns, b->columns));
	Assert(a->ndefaults == b->ndefaults && a->nconstraints == b->nconstraints);
	for (int i = 0; i < a->ndefaults; i++)
		Assert(equal(a->defaults[i], b->defaults[i]));
	for (int i = 0; i < a->nconstraints; i++)
		Assert(equal(a->constraints[i], b->constraints[i]));
	Assert(equal(a->index, b->index));
	Assert(a->nopclasses == b->nopclasses);
	for (int i = 0; i < a->nopclasses; i++)
		Assert(a->opclass_types[i] == b->opclass_types[i] && a->opclasses[i] == b->opclasses[i]);
	MemoryContextDelete(copy->memory);
}
#endif

static int
used_compare(const void *a, const void *b)
{
	return pg_cmp_u64(*(const uint64 *) a, *(const uint64 *) b);
}

/* Evict the least recently used half of the shared entries. */
static void
shared_evict(void)
{
	dshash_seq_status status;
	SharedSchema *shared;
	uint64	   *used;
	int			n = 0,
				capacity = 256;

	if (!pg_atomic_test_set_flag(&shared_control->evicting))
		return;
	PG_TRY();
	{
		used = palloc_array(uint64, capacity);
		dshash_seq_init(&status, shared_schemas, false);
		while ((shared = dshash_seq_next(&status)) != NULL)
		{
			if (n == capacity)
				used = repalloc_array(used, uint64, capacity *= 2);
			used[n++] = shared->used;
		}
		dshash_seq_term(&status);
		if (n > 0)
		{
			uint64		cutoff;

			qsort(used, n, sizeof(uint64), used_compare);
			cutoff = used[(n - 1) / 2];
			dshash_seq_init(&status, shared_schemas, true);
			while ((shared = dshash_seq_next(&status)) != NULL)
				if (shared->used <= cutoff)
				{
					dsa_free(shared_area, shared->data);
					pg_atomic_sub_fetch_u64(&shared_control->bytes, shared->size);
					dshash_delete_current(&status);
				}
			dshash_seq_term(&status);
		}
		pfree(used);
	}
	PG_FINALLY();
	{
		pg_atomic_clear_flag(&shared_control->evicting);
	}
	PG_END_TRY();
}

/* Share an entry, unless the shared copy already has all its parts. */
static void
shared_publish(CachedSchema *entry)
{
	StringInfoData buf;
	SharedSchema *shared;
	dsa_pointer data;
	bool		found;

	if (!shared_attach())
		return;
	schema_serialize(&entry->schema, &buf);
#ifdef USE_ASSERT_CHECKING
	schema_check_serialized(entry, &buf);
#endif
	shared = dshash_find_or_insert(shared_schemas, &entry->key, &found);
	if (found && (shared->parts & entry->schema.parts) == entry->schema.parts)
	{
		dshash_release_lock(shared_schemas, shared);
		pfree(buf.data);
		return;
	}
	data = dsa_allocate_extended(shared_area, buf.len, DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(data))
	{
		if (found)
			dshash_release_lock(shared_schemas, shared);
		else
			dshash_delete_entry(shared_schemas, shared);
		pfree(buf.data);
		return;
	}
	memcpy(dsa_get_address(shared_area, data), buf.data, buf.len);
	if (found)
	{
		dsa_free(shared_area, shared->data);
		pg_atomic_sub_fetch_u64(&shared_control->bytes, shared->size);
	}
	shared->data = data;
	shared->size = buf.len;
	shared->parts = entry->schema.parts;
	shared->used = pg_atomic_fetch_add_u64(&shared_control->clock, 1);
	dshash_release_lock(shared_schemas, shared);
	pfree(buf.data);
	if (pg_atomic_add_fetch_u64(&shared_control->bytes, buf.len) > (uint64) dolt_shared_schema_cache_size * 1024)
		shared_evict();
}

static CachedSchema *
shared_read(const SchemaKey *key)
{
	SharedSchema *shared;
	CachedSchema *entry;
	char	   *data;
	Size		len;

	if (!shared_attach())
		return NULL;
	shared = dshash_find(shared_schemas, key, false);
	if (!shared)
		return NULL;
	shared->used = pg_atomic_fetch_add_u64(&shared_control->clock, 1);
	len = shared->size;
	data = palloc(len);
	memcpy(data, dsa_get_address(shared_area, shared->data), len);
	dshash_release_lock(shared_schemas, shared);
	entry = schema_deserialize(key, data, len, schema_memory);
	pfree(data);
	return entry;
}

static void
local_measure(CachedSchema *entry)
{
	if (entry->cached)
		local_bytes -= entry->size;
	entry->size = MemoryContextMemAllocated(entry->memory, true);
	if (entry->cached)
		local_bytes += entry->size;
}

static void
local_insert(CachedSchema *entry)
{
	LocalSchema *local = hash_search(local_schemas, &entry->key, HASH_ENTER, NULL);
	TextSchema *text;
	bool		found;

	local->entry = entry;
	text = hash_search(text_schemas, &entry->schema.json, HASH_ENTER, &found);
	if (!found)
		text->entry = entry;
	entry->cached = true;
	dlist_push_head(&schema_lru, &entry->lru);
	local_measure(entry);
}

DGSchema *
dg_schema_get(bytea *address, const char *kind)
{
	SchemaKey	key = {0};
	LocalSchema *local;
	CachedSchema *entry;

	local_init();
	if (VARSIZE_ANY_EXHDR(address) != PL_HASH_SIZE || strlen(kind) >= sizeof(key.kind))
	{
		/* Not an address this cache can key: decode it for this caller. */
		entry = schema_create(&key, CurrentMemoryContext);
		entry->schema.json = MemoryContextStrdup(entry->memory, dg_schema_decode(address, kind));
		entry->schema.document = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(entry->schema.json)));
		return &entry->schema;
	}
	memcpy(key.address, VARDATA_ANY(address), PL_HASH_SIZE);
	strcpy(key.kind, kind);
	key.encoding = GetDatabaseEncoding();
	local = hash_search(local_schemas, &key, HASH_FIND, NULL);
	if (local)
	{
		dlist_move_head(&schema_lru, &local->entry->lru);
		return &local->entry->schema;
	}
	entry = shared_read(&key);
	if (!entry)
	{
		char	   *json = dg_schema_decode(address, kind);
		MemoryContext previous;

		entry = schema_create(&key, schema_memory);
		previous = MemoryContextSwitchTo(entry->memory);
		entry->schema.json = pstrdup(json);
		entry->schema.document = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
		MemoryContextSwitchTo(previous);
		shared_publish(entry);
	}
	local_insert(entry);
	return &entry->schema;
}

char *
dg_schema_read(bytea *address, const char *kind)
{
	return pstrdup(dg_schema_get(address, kind)->json);
}

/* The cached entry whose definition is |json|, if any. */
DGSchema *
dg_schema_find(const char *json)
{
	TextSchema *text;

	if (!text_schemas)
		return NULL;
	text = hash_search(text_schemas, &json, HASH_FIND, NULL);
	if (!text)
		return NULL;
	dlist_move_head(&schema_lru, &text->entry->lru);
	return &text->entry->schema;
}

/* Note a part derived from an entry's definition. */
static void
schema_derived(CachedSchema *entry, uint32 part)
{
	entry->schema.parts |= part;
	local_measure(entry);
	if (entry->cached)
		shared_publish(entry);
}

/* Keep |columns|, whose types and collations must all be built in. */
void
dg_schema_set_columns(DGSchema *schema, TupleDesc columns)
{
	CachedSchema *entry = (CachedSchema *) schema;

	if (schema->parts & DG_SCHEMA_COLUMNS)
		return;
	Assert(!columns->constr);
	schema->columns = MemoryContextAlloc(entry->memory, TupleDescSize(columns));
	memcpy(schema->columns, columns, TupleDescSize(columns));
	schema->columns->tdrefcount = -1;
	schema_derived(entry, DG_SCHEMA_COLUMNS);
}

/* Keep raw parse trees, which must name only built-in objects or names. */
void
dg_schema_set_nodes(DGSchema *schema, uint32 part, int count, Node **nodes)
{
	CachedSchema *entry = (CachedSchema *) schema;
	MemoryContext previous;
	Node	  **copies;

	if (schema->parts & part)
		return;
	previous = MemoryContextSwitchTo(entry->memory);
	copies = palloc0_array(Node *, Max(count, 1));
	for (int i = 0; i < count; i++)
		copies[i] = copyObject(nodes[i]);
	MemoryContextSwitchTo(previous);
	switch (part)
	{
		case DG_SCHEMA_DEFAULTS:
			schema->ndefaults = count;
			schema->defaults = copies;
			break;
		case DG_SCHEMA_CONSTRAINTS:
			schema->nconstraints = count;
			schema->constraints = copies;
			break;
		case DG_SCHEMA_INDEX:
			Assert(count == 1);
			schema->index = copies[0];
			break;
		default:
			elog(ERROR, "unrecognized schema part %u", part);
	}
	schema_derived(entry, part);
}

/* Keep the operator classes resolved for an index's keys, all built in. */
void
dg_schema_set_opclasses(DGSchema *schema, int count, Oid *types, Oid *opclasses)
{
	CachedSchema *entry = (CachedSchema *) schema;

	if (schema->parts & DG_SCHEMA_OPCLASSES)
		return;
	schema->nopclasses = count;
	schema->opclass_types = MemoryContextAlloc(entry->memory, sizeof(Oid) * Max(count, 1));
	schema->opclasses = MemoryContextAlloc(entry->memory, sizeof(Oid) * Max(count, 1));
	memcpy(schema->opclass_types, types, sizeof(Oid) * count);
	memcpy(schema->opclasses, opclasses, sizeof(Oid) * count);
	schema_derived(entry, DG_SCHEMA_OPCLASSES);
}
