# src/test/modules/test_dolt_chunk/Makefile

MODULE_big = test_dolt_chunk
OBJS = \
	$(WIN32RES) \
	test_dolt_chunk.o
PGFILEDESC = "test_dolt_chunk - exercise and benchmark the dolt chunk store"

EXTENSION = test_dolt_chunk
DATA = test_dolt_chunk--1.0.sql

REGRESS = test_dolt_chunk
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_dolt_chunk
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
