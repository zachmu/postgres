# Standalone prototype: no configured PostgreSQL build or backend required.
CC ?= cc
AR ?= ar
CPPFLAGS += -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags openssl)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes -Wstrict-prototypes
LDLIBS += $(shell pkg-config --libs openssl)
OBJECTS = file_store.o store.o tree.o tuple.o version.o

.PHONY: all check sanitize clean
all: libprolly.a
libprolly.a: $(OBJECTS)
	$(AR) rcs $@ $^
$(OBJECTS) test_prolly.o test_file_store.o: prolly.h internal.h
test_prolly: test_prolly.o libprolly.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_prolly.o libprolly.a $(LDLIBS)
test_file_store: test_file_store.o libprolly.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_file_store.o libprolly.a $(LDLIBS)
check: test_prolly test_file_store
	./test_prolly
	./test_prolly --file
	./test_file_store
sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes -Wstrict-prototypes -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
	$(MAKE) clean
clean:
	rm -f $(OBJECTS) test_prolly.o libprolly.a test_prolly test_file_store test_file_store.o
