cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME cardinal)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/cardinal_extension.cpp
    src/scan_hook.cpp
    src/exact.cpp
    src/inner_query.cpp
    src/model.cpp
    src/model_store.cpp
    src/store.cpp
    src/models.cpp
    src/bn_estimator.cpp
    src/fit.cpp
    src/fit_function.cpp
    src/query_graph.cpp
    src/keys.cpp
    src/join_estimator.cpp
    src/join_planner.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
