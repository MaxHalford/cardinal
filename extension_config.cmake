# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(cardinal
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
# Since v2, a loaded extension is only built: link it into build/release/duckdb too.
duckdb_extension_statically_link(cardinal)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)