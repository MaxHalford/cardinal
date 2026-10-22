PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=cardinal
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# DuckDB v2 stamps a build with its commit unless given its release, e.g. v2.0.0:
# extensions then load only into that build, not into the released DuckDB.
export DUCKDB_VERSION ?= $(shell git -C duckdb describe --tags --exact-match 2>/dev/null)

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile