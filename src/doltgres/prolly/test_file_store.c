#include "internal.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == PL_OK)

static PLSlice
text		(const char *s)
{
	PLSlice		result = {(const uint8_t *) s, strlen(s)};

	return result;
}

static off_t
file_size(const char *path)
{
	struct stat info;

	CHECK(stat(path, &info) == 0);
	return info.st_size;
}

static void
replace_file(const char *path, const uint8_t *data, size_t length)
{
	int			fd = open(path, O_WRONLY | O_TRUNC);

	CHECK(fd >= 0);
	CHECK(write(fd, data, length) == (ssize_t) length);
	CHECK(close(fd) == 0);
}

typedef struct Gate
{
	const char *path;
	off_t		expected;
	unsigned	calls;
	bool		reject;
	PLHash		hash;
	uint8_t    *data;
	size_t		length;
} Gate;

static PLStatus
before_write(void *arg, PLHash hash, PLSlice bytes)
{
	Gate	   *gate = arg;

	CHECK(file_size(gate->path) == gate->expected);
	gate->calls++;
	free(gate->data);
	gate->data = malloc(bytes.len);
	CHECK(gate->data);
	memcpy(gate->data, bytes.data, bytes.len);
	gate->length = bytes.len;
	gate->hash = hash;
	return gate->reject ? PL_IO : PL_OK;
}

static void
lifecycle(const char *path)
{
	PLStore    *store = NULL,
			   *other = NULL;
	PLTree		base,
				left,
				right,
				merged;
	PLHash		old,
				final;
	PLEntry		entry;
	PLEdit		edit;
	PLValue		value;
	entry.key = text ("a");
	entry.value = text ("original");
	edit.key = text ("b");

	edit.value.present = true;
	edit.value.bytes = text ("left");

	OK(pl_store_open_file(path, false, NULL, NULL, &store));
	CHECK(pl_store_open_file(path, false, NULL, NULL, &other) == PL_BUSY);
	CHECK(other == NULL);
	OK(pl_build(store, &entry, 1, &base));
	old = base.root;
	left = pl_branch(&base);
	OK(pl_apply(&left, &edit, 1, &left));
	edit.key = text ("c");
	edit.value.bytes = text ("right");

	OK(pl_apply(&base, &edit, 1, &right));
	OK(pl_merge(&base, &left, &right, NULL, NULL, &merged));
	final = merged.root;
	OK(pl_store_sync(store));
	pl_store_free(store);
	OK(pl_store_open_file(path, false, NULL, NULL, &store));
	OK(pl_open(store, old, &base));
	OK(pl_open(store, final, &merged));
	CHECK(pl_count(&base) == 1 && pl_count(&merged) == 3);
	OK(pl_get(&merged, text ("c"), &value));
	CHECK(pl_compare(value.bytes, text ("right")) == 0);
	pl_store_free(store);
}

static void
prefixes_and_replay(const char *path)
{
	PLStore    *store = NULL;
	PLTree		tree;
	PLEntry		entry;
	Gate		gate = {0};
	PLSlice		chunk;
	uint8_t    *record;
	size_t		length,
				i;
	int			fd;
	entry.key = text ("x");
	entry.value = text ("payload");

	gate.path = path;
	replace_file(path, NULL, 0);
	OK(pl_store_open_file(path, false, before_write, &gate, &store));
	gate.reject = true;
	CHECK(pl_build(store, &entry, 1, &tree) == PL_IO);
	CHECK(file_size(path) == 0 && pl_store_stats(store).chunks == 0);
	gate.reject = false;
	OK(pl_build(store, &entry, 1, &tree));
	CHECK(gate.calls == 2);
	gate.expected = file_size(path);
	OK(pl_build(store, &entry, 1, &tree));
	CHECK(gate.calls == 2 && file_size(path) == gate.expected);
	OK(pl_store_sync(store));
	pl_store_free(store);
	length = (size_t) file_size(path);
	record = malloc(length);
	CHECK(record);
	fd = open(path, O_RDONLY);
	CHECK(fd >= 0 && read(fd, record, length) == (ssize_t) length);
	CHECK(close(fd) == 0);
	chunk.data = gate.data;
	chunk.len = gate.length;

	/*
	 * Every incomplete record prefix: strict open rejects, explicit repair
	 * truncates, then replay restores exactly the original chunk and root.
	 */
	for (i = 0; i < length; i++)
	{
		replace_file(path, record, i);
		store = NULL;
		if (i)
		{
			CHECK(pl_store_open_file(path, false, NULL, NULL, &store) == PL_CORRUPT);
			CHECK(store == NULL && file_size(path) == (off_t) i);
		}
		OK(pl_store_open_file(path, true, before_write, &gate, &store));
		CHECK(file_size(path) == 0);
		OK(pl_store_replay(store, gate.hash, chunk));
		OK(pl_store_replay(store, gate.hash, chunk));
		CHECK(gate.calls == 2 && file_size(path) == (off_t) length);
		OK(pl_open(store, gate.hash, &tree));
		OK(pl_store_sync(store));
		pl_store_free(store);
	}
	/* A valid prefix survives repair of a partial following record. */
	fd = open(path, O_WRONLY | O_APPEND);
	CHECK(fd >= 0 && write(fd, record, 17) == 17);
	CHECK(close(fd) == 0);
	OK(pl_store_open_file(path, true, NULL, NULL, &store));
	CHECK(file_size(path) == (off_t) length);
	OK(pl_open(store, gate.hash, &tree));
	pl_store_free(store);
	/* A complete damaged record is never silently repaired. */
	record[length - 1] ^= 1;
	replace_file(path, record, length);
	store = NULL;
	CHECK(pl_store_open_file(path, true, NULL, NULL, &store) == PL_CORRUPT);
	CHECK(store == NULL && file_size(path) == (off_t) length);
	record[length - 1] ^= 1;
	record[15] ^= 1;			/* Length corruption must not masquerade as a
								 * torn tail. */
	replace_file(path, record, length);
	CHECK(pl_store_open_file(path, true, NULL, NULL, &store) == PL_CORRUPT);
	CHECK(file_size(path) == (off_t) length);
	free(record);
	free(gate.data);
}

static void
io_failure(const char *path)
{
	PLStore    *store = NULL;
	PLTree		tree;
	int			readonly;
	int			pipefd[2];

	replace_file(path, NULL, 0);
	OK(pl_store_open_file(path, false, NULL, NULL, &store));

	/*
	 * Force EBADF on write without relying on filesystem capacity or
	 * privileges.
	 */
	readonly = open(path, O_RDONLY);
	CHECK(readonly >= 0 && dup2(readonly, store->fd) == store->fd);
	CHECK(close(readonly) == 0);
	CHECK(pl_build(store, NULL, 0, &tree) == PL_IO);
	CHECK(pl_store_stats(store).chunks == 0);
	CHECK(pl_store_sync(store) == PL_IO);
	CHECK(pl_build(store, NULL, 0, &tree) == PL_IO);
	pl_store_free(store);
	OK(pl_store_open_file(path, false, NULL, NULL, &store));
	CHECK(pipe(pipefd) == 0);
	CHECK(dup2(pipefd[1], store->fd) == store->fd);
	CHECK(close(pipefd[1]) == 0);
	/* Pipes reject fsync: a durability error must also poison the handle. */
	CHECK(pl_store_sync(store) == PL_IO);
	CHECK(pl_build(store, NULL, 0, &tree) == PL_IO);
	CHECK(close(pipefd[0]) == 0);
	pl_store_free(store);
}

static PLStatus
count_diff(void *arg, PLSlice key, PLValue before, PLValue after)
{
	unsigned   *count = arg;

	(void) key;
	CHECK(before.present && after.present);
	(*count)++;
	return PL_OK;
}

static void
large_reopen(const char *path)
{
	PLStore    *store = NULL;
	PLTree		original,
				changed;
	PLHash		roots[2];
	PLEntry		entries[1000];
	uint8_t		keys[1000][8];
	PLEdit		edit;
	unsigned	count = 0;
	size_t		i;

	replace_file(path, NULL, 0);
	for (i = 0; i < 1000; i++)
	{
		pl_key_int64((int64_t) i, keys[i]);
		entries[i].key.data = keys[i];
		entries[i].key.len = 8;
		entries[i].value = text ("tuple contents");
	}
	OK(pl_store_open_file(path, false, NULL, NULL, &store));
	OK(pl_build(store, entries, 1000, &original));
	edit.key = entries[500].key;
	edit.value.present = true;
	edit.value.bytes = text ("changed");

	OK(pl_apply(&original, &edit, 1, &changed));
	roots[0] = original.root;
	roots[1] = changed.root;
	OK(pl_store_sync(store));
	pl_store_free(store);
	OK(pl_store_open_file(path, false, NULL, NULL, &store));
	OK(pl_open(store, roots[0], &original));
	OK(pl_open(store, roots[1], &changed));
	CHECK(pl_count(&original) == 1000 && pl_count(&changed) == 1000);
	OK(pl_diff(&original, &changed, count_diff, &count));
	CHECK(count == 1);
	pl_store_free(store);
}

static void
process_exit(const char *path)
{
	PLStore    *store = NULL;
	PLTree		tree;
	PLHash		root;
	pid_t		child;
	int			status,
				pipefd[2];

	replace_file(path, NULL, 0);
	CHECK(pipe(pipefd) == 0);
	child = fork();
	CHECK(child >= 0);
	if (!child)
	{
		close(pipefd[0]);
		if (pl_store_open_file(path, false, NULL, NULL, &store) != PL_OK ||
			pl_build(store, NULL, 0, &tree) != PL_OK || pl_store_sync(store) != PL_OK ||
			write(pipefd[1], &tree.root, sizeof(root)) != sizeof(root))
			_exit(1);
		_exit(0);				/* No close/free: another process must recover
								 * the file. */
	}
	CHECK(close(pipefd[1]) == 0);
	CHECK(read(pipefd[0], &root, sizeof(root)) == sizeof(root));
	CHECK(close(pipefd[0]) == 0);
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	OK(pl_store_open_file(path, false, NULL, NULL, &store));
	OK(pl_open(store, root, &tree));
	CHECK(pl_count(&tree) == 0);
	pl_store_free(store);
}

int
main(void)
{
	char		path[] = "/tmp/prolly-durable-XXXXXX";
	int			fd = mkstemp(path);

	CHECK(fd >= 0 && close(fd) == 0);
	lifecycle(path);
	large_reopen(path);
	prefixes_and_replay(path);
	io_failure(path);
	process_exit(path);
	CHECK(unlink(path) == 0);
	printf("file store passed %u checks\n", checks);
	return 0;
}
