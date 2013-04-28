EXTENSION    = json_build
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DATA         = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS         = $(wildcard doc/*.md)
USE_MODULE_DB = 1
TESTS        = $(wildcard test/sql/*.sql)
REGRESS_OPTS = --inputdir=test --outputdir=test \
	  --load-extension=$(EXTENSION)\
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
MODULE_big      = $(EXTENSION)
OBJS         = $(patsubst src/%.c,src/%.o,$(wildcard src/*.c))
PG_CONFIG    = pg_config

all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

DATA = $(wildcard sql/*--*.sql)
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# we put all the tests in a test subdir, but pgxs expects us not to, darn it
override pg_regress_clean_files = test/results/ test/regression.diffs test/regression.out tmp_check/ log/
