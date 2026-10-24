# cardinal for PostgreSQL, built with PGXS: make PG_CONFIG=/path/to/pg_config
MODULE_big = cardinal
OBJS = cardinal.o joinrels.o

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
