# Ensure the bench module uses C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(./common ./interface)

# ------------------------------------------------------------
# Backend selection
#
# Default to TREEPASS; pass -Dbackend=CLOCK1 (etc.) to link against a
# baseline. Every backend exports the same SplinterDB public API, so
# the bench code itself doesn't change.
#
#   backend=TREEPASS  -> ../splinterdb/                       (this repo)
#   backend=CLOCK1    -> ../third_party/splinterdb-clock1/    (1-bit CLOCK)
#   backend=CLOCK2    -> ../third_party/splinterdb-clock2/    (2-bit CLOCK)
#   backend=CLOCK3    -> ../third_party/splinterdb-clock3/    (3-bit CLOCK)
#   backend=S3FIFO    -> ../third_party/splinterdb-s3fifo/    (S3-FIFO)
# ------------------------------------------------------------
if(NOT DEFINED backend)
  set(backend "TREEPASS")
endif()

set(_BACKEND_DIR "")
if("${backend}" STREQUAL "TREEPASS")
  set(_BACKEND_DIR ${treepass_SOURCE_DIR})
elseif("${backend}" STREQUAL "CLOCK1")
  set(_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/splinterdb-clock1)
elseif("${backend}" STREQUAL "CLOCK2")
  set(_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/splinterdb-clock2)
elseif("${backend}" STREQUAL "CLOCK3")
  set(_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/splinterdb-clock3)
elseif("${backend}" STREQUAL "S3FIFO")
  set(_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/splinterdb-s3fifo)
else()
  message(FATAL_ERROR "Unknown backend '${backend}'. "
                      "Expected TREEPASS|CLOCK1|CLOCK2|CLOCK3|S3FIFO.")
endif()

message(STATUS "Bench backend: ${backend} (dir=${_BACKEND_DIR})")

set(_BACKEND_INCLUDE_DIR  ${_BACKEND_DIR}/include)
set(_BACKEND_SRC_DIR      ${_BACKEND_DIR}/src)
set(_BACKEND_PLATFORM_DIR ${_BACKEND_DIR}/src/platform_linux)

# BUILD_ROOT may be set per-backend so concurrent builds of different
# backends don't fight over a single build/release dir.
if(DEFINED ENV{BUILD_ROOT})
  set(_BACKEND_BUILD_ROOT $ENV{BUILD_ROOT})
else()
  set(_BACKEND_BUILD_ROOT ${_BACKEND_DIR}/build)
endif()

# SplinterDB Makefile lays out build/<MODE>[-asan][-ubsan]/lib/libsplinterdb.a
if("${BUILD_MODE}" STREQUAL "debug")
  set(_BACKEND_BUILD_SUBDIR "debug")
else()
  set(_BACKEND_BUILD_SUBDIR "release")
endif()
if(BUILD_ASAN AND NOT "${BUILD_ASAN}" STREQUAL "0")
  set(_BACKEND_BUILD_SUBDIR "${_BACKEND_BUILD_SUBDIR}-asan")
endif()
if(BUILD_UBSAN AND NOT "${BUILD_UBSAN}" STREQUAL "0")
  set(_BACKEND_BUILD_SUBDIR "${_BACKEND_BUILD_SUBDIR}-ubsan")
endif()

set(_BACKEND_LIBRARY ${_BACKEND_BUILD_ROOT}/${_BACKEND_BUILD_SUBDIR}/lib/libsplinterdb.a)
message(STATUS "Bench backend library: ${_BACKEND_LIBRARY}")

# ------------------------------------------------------------
# Convenience: every bench binary needs the same set of includes,
# libraries, link options, etc. Wrap that in a function.
# ------------------------------------------------------------
function(add_bench_binary name)
  add_executable(${name} ${name}.cpp)
  target_compile_definitions(${name} PUBLIC SPLT)
  target_include_directories(${name} PUBLIC
      ${global_INCLUDE_DIR}
      ${_BACKEND_INCLUDE_DIR}
      ${_BACKEND_SRC_DIR}
      ${_BACKEND_PLATFORM_DIR}
  )
  target_link_libraries(${name}
      # bench-direct
      gflags pthread
      # the splinterdb backend itself (static lib)
      ${_BACKEND_LIBRARY}
      # transitive: splinterdb's laio.c uses libaio, routing filter etc. use
      # libxxhash, libm is pulled in by math calls.
      m aio xxhash
  )
  if("${BUILD_ASAN}" STREQUAL "1")
    target_link_options(${name} PUBLIC -fsanitize=address)
  endif()
  if("${BUILD_UBSAN}" STREQUAL "1")
    target_link_options(${name} PUBLIC -fsanitize=undefined)
  endif()
endfunction()

# ------------------------------------------------------------
# Bench binaries
# ------------------------------------------------------------
add_bench_binary(ycsb_bench)
