# Consumer example, built two ways from this one file:
#  - as part of the rapidproto build (add_subdirectory from the top-level CMakeLists under
#    RAPIDPROTO_BUILD_TESTS) -- the in-tree mirror of a FetchContent consumer;
#  - STANDALONE against an INSTALLED rapidproto (configure this directory directly with
#    CMAKE_PREFIX_PATH at the install prefix) -- the find_package consumer, which CI builds so the
#    installed packaging surface (Config file, imported tool, helper) can't regress silently.
# Either way it exercises rapidproto_generate() end to end: a cross-file schema is code-generated,
# compiled into an executable, and run as a CTest that decodes a buffer.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.21)  # rapidproto_generate uses DEPFILE (3.20+ Makefiles, 3.21+ VS/Xcode)
  project(rapidproto_consumer_example CXX)
  enable_testing()
  find_package(rapidproto REQUIRED)
endif()

# One call turns the schema into a linkable, header-only target. GENERATOR both emits the arena AND the
# streaming decoder (+ the shared common header) from a SINGLE rapidprotoc invocation -- one custom
# command with both headers as OUTPUT and one multi-target depfile. message.proto imports types.proto,
# so the generated closure (and the depfile that tracks it) is exercised too.
rapidproto_generate(rapidproto_example_schema
  GENERATOR both
  PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/message.proto
  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto)

add_executable(rapidproto_example_consumer main.cpp)
target_compile_features(rapidproto_example_consumer PRIVATE cxx_std_17)
# Linking the generated target both triggers codegen before this compiles and adds its OUT_DIR to the
# include path -- the output is self-contained (the CLI drops the runtime there), so nothing else needed.
target_link_libraries(rapidproto_example_consumer PRIVATE rapidproto_example_schema)

add_test(NAME rapidproto_example_decode COMMAND rapidproto_example_consumer)
