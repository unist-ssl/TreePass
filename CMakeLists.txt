cmake_minimum_required(VERSION 3.17)
project(BASELINE)

set(CMAKE_CXX_STANDARD 17)

set(global_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# TreePass (vendored in this repo): the cache replacement policy under evaluation.
set(treepass_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/splinterdb)
set(treepass_INCLUDE_DIR ${treepass_SOURCE_DIR}/include)
add_definitions(-DSPLINTERDB_PLATFORM_DIR=platform_linux)

# gflags (required by bench drivers)
option(GFLAGS_USE_TARGET_NAMESPACE "Use gflags import target with namespace." ON)
find_package(gflags CONFIG)
if(gflags_FOUND)
  if(TARGET ${GFLAGS_TARGET})
    set(GFLAGS_LIB ${GFLAGS_TARGET})
  else()
    set(GFLAGS_LIB ${gflags_LIBRARIES})
  endif()
else()
  find_package(gflags REQUIRED)
  set(GFLAGS_LIB gflags::gflags)
endif()
include_directories(${GFLAGS_INCLUDE_DIR})
list(APPEND THIRDPARTY_LIBS ${GFLAGS_LIB})
add_definitions(-DGFLAGS=1)

add_subdirectory(bench)
