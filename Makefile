EXTENSION    = json_build

ifeq ($(wildcard vpath.mk),vpath.mk)
include vpath.mk
else
ext_srcdir = .
endif

EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
DATA         = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS         = $(wildcard $(ext_srcdir)/doc/*.md)
USE_MODULE_DB = 1
TESTS        = $(wildcard $(ext_srcdir)/test/sql/*.sql)
REGRESS_OPTS = --inputdir=$(ext_srcdir)/test --outputdir=test \
	--load-extension=$(EXTENSION)
REGRESS      = $(patsubst $(ext_srcdir)/test/sql/%.sql,%,$(TESTS))
MODULE_big      = $(EXTENSION)
OBJS         = $(patsubst $(ext_srcdir)/src/%.c,src/%.o,$(wildcard $(ext_srcdir)/src/*.c))
PG_CONFIG    = pg_config

all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: $(ext_srcdir)/sql/$(EXTENSION).sql
	cp $< $@

DATA_built = sql/$(EXTENSION)--$(EXTVERSION).sql
DATA = $(filter-out $(ext_srcdir)/sql/$(EXTENSION)--$(EXTVERSION).sql, $(wildcard $(ext_srcdir)/sql/*--*.sql))
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# we put all the tests in a test subdir, but pgxs expects us not to, darn it
override pg_regress_clean_files = test/results/ test/regression.diffs test/regression.out tmp_check/ log/
