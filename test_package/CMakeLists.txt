cmake_minimum_required(VERSION 3.21)  # rapidproto_generate uses DEPFILE
project(rapidproto_test_package CXX)
enable_testing()

find_package(rapidproto REQUIRED)

rapidproto_generate(point_schema
  PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/point.proto
  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto)

add_executable(tp_consumer main.cpp)
target_compile_features(tp_consumer PRIVATE cxx_std_17)
target_link_libraries(tp_consumer PRIVATE point_schema)

add_test(NAME tp_decode COMMAND tp_consumer)
