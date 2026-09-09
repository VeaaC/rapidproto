# OSM PBF showcase: decode real OpenStreetMap planet-format data with both models. Two
# executables from one generated schema batch, deliberately SEPARATE so a reader studies one
# model at a time:
#   osmstat-arena  -- materialize each PrimitiveBlock, walk the arrays (osmstat_arena.cpp)
#   osmstat-stream -- callback walks, nothing materialized (osmstat_stream.cpp)
# Both print identical statistics for the same input (a CTest holds them to that), timing to
# stderr. Built like the consumer example: in-tree under RAPIDPROTO_BUILD_TESTS, or standalone
# against an installed rapidproto. zlib is a dependency of THIS EXAMPLE (PBF blobs are
# zlib-deflated), not of the library; without it the example is skipped with a notice.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.21)  # rapidproto_generate uses DEPFILE (3.20+ Makefiles, 3.21+ VS/Xcode)
  project(rapidproto_osm_pbf_example CXX)
  enable_testing()
  find_package(rapidproto REQUIRED)
endif()

find_package(ZLIB)
if(NOT ZLIB_FOUND)
  message(STATUS "rapidproto: zlib not found -- skipping the osm-pbf example")
  return()
endif()

# One batch, one OUT_DIR: osmformat.proto and fileformat.proto are independent schemas, but a
# translation unit may include generated headers from only ONE output tree (each tree carries its
# own runtime copy), so they must be generated together.
rapidproto_generate(rapidproto_osmpbf_schema
  GENERATOR both
  PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/fileformat.proto
         ${CMAKE_CURRENT_SOURCE_DIR}/proto/osmformat.proto
  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto)

foreach(model arena stream)
  add_executable(osmstat-${model} osmstat_${model}.cpp)
  target_compile_features(osmstat-${model} PRIVATE cxx_std_17)
  target_link_libraries(osmstat-${model} PRIVATE rapidproto_osmpbf_schema ZLIB::ZLIB)
endforeach()

# The committed fixture must reproduce from its generator (same discipline as the goldens), and
# both models must agree byte-for-byte on the statistics they print for it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME rapidproto_osmstat_fixture
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/testdata/check_fixture.py
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/mini.osm.pbf
            $<TARGET_FILE:osmstat-arena> $<TARGET_FILE:osmstat-stream>)
else()
  message(STATUS "rapidproto: python3 not found -- the osmstat fixture test will NOT run")
endif()
