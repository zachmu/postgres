# src/test/modules/test_doltgres/Makefile
MODULE_big = test_doltgres
OBJS = $(WIN32RES) test_doltgres.o
PGFILEDESC = "test_doltgres - branch catalog projection experiment"
REGRESS = branch_catalog
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_doltgres
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
