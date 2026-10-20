.DEFAULT_GOAL := all
MODULE_big = dolt_prolly
OBJS = dg_store.o dg_table.o dg_index.o pl_store.o pl_tree.o pl_tuple.o pl_version.o pl_file.o
EXTENSION = dolt_prolly
DATA = dolt_prolly--1.0.sql
SHLIB_LINK = -lcrypto
PG_CPPFLAGS = -I$(srcdir)/../../src/doltgres/prolly
$(OBJS): dg.h ../../src/doltgres/prolly/prolly.h ../../src/doltgres/prolly/internal.h
REGRESS = storage
TAP_TESTS = 1
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/dolt_prolly
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
