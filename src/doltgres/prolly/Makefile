# Standalone prototype: no configured PostgreSQL build or backend required.
CC ?= cc
AR ?= ar
CPPFLAGS += $(shell pkg-config --cflags openssl)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell pkg-config --libs openssl)
OBJECTS = store.o tree.o tuple.o version.o

.PHONY: all check sanitize clean
all: libprolly.a
libprolly.a: $(OBJECTS)
	$(AR) rcs $@ $^
$(OBJECTS) test_prolly.o: prolly.h internal.h
test_prolly: test_prolly.o libprolly.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_prolly.o libprolly.a $(LDLIBS)
check: test_prolly
	./test_prolly
sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
clean:
	rm -f $(OBJECTS) test_prolly.o libprolly.a test_prolly
