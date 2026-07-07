# rdma_transport — W5 RDMA (RoCEv2) foundation + tools.
#
# Builds standalone (no dependency on the main Embarcadero build) so Track 05 can iterate on the
# fabric without touching shared CMake. Integration into the main build is a later, guarded step:
#   add_subdirectory(rdma_transport) in src/CMakeLists.txt, gated on find_library(ibverbs).
#
# Standalone build on broker:
#   cmake -S src/rdma_transport -B build_rdma && cmake --build build_rdma -j 8
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_NAME)
  project(embarcadero_rdma_transport CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  add_compile_options(-Wall -O3)
endif()

find_library(IBVERBS_LIB ibverbs)
if(NOT IBVERBS_LIB)
  message(FATAL_ERROR "libibverbs not found (install libibverbs-dev)")
endif()

# Include root so "rdma_transport/rdma_common.h" resolves (matches the main tree's include style).
get_filename_component(_RDMA_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(rdma_transport STATIC rdma_common.cc)
target_include_directories(rdma_transport PUBLIC ${_RDMA_SRC_ROOT})
target_link_libraries(rdma_transport PUBLIC ${IBVERBS_LIB})

add_executable(rdma_bench rdma_bench_main.cc)
target_link_libraries(rdma_bench PRIVATE rdma_transport)
set_target_properties(rdma_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- W5-A / W5-B variant harness (docs/design/W5_RDMA_VARIANTS_SPEC.md) --------------------------
# Passive memory server: always hosts metadata (Decision 1); --host-blog additionally hosts the
# payload Blog (W5-B). One binary serves both variants at runtime.
add_executable(rdma_memserver rdma_memserver.cc)
target_link_libraries(rdma_memserver PRIVATE rdma_transport)
set_target_properties(rdma_memserver PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Broker/producer + sequencer: ONE TU each, TWO targets differing only by -DBLOG_PLACEMENT_DRAM
# (spec §4 CI check: "both targets build from the same TU set differing only by the macro").
add_executable(w5_broker_dram rdma_broker.cc)
target_link_libraries(w5_broker_dram PRIVATE rdma_transport)
target_compile_definitions(w5_broker_dram PRIVATE BLOG_PLACEMENT_DRAM=1)
set_target_properties(w5_broker_dram PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(w5_broker_memserver rdma_broker.cc)
target_link_libraries(w5_broker_memserver PRIVATE rdma_transport)
target_compile_definitions(w5_broker_memserver PRIVATE BLOG_PLACEMENT_DRAM=0)
set_target_properties(w5_broker_memserver PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(w5_sequencer_dram rdma_sequencer.cc)
target_link_libraries(w5_sequencer_dram PRIVATE rdma_transport)
target_compile_definitions(w5_sequencer_dram PRIVATE BLOG_PLACEMENT_DRAM=1)
set_target_properties(w5_sequencer_dram PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(w5_sequencer_memserver rdma_sequencer.cc)
target_link_libraries(w5_sequencer_memserver PRIVATE rdma_transport)
target_compile_definitions(w5_sequencer_memserver PRIVATE BLOG_PLACEMENT_DRAM=0)
set_target_properties(w5_sequencer_memserver PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- Reviewer-D naive RDMA token server repro (spec §5, E10 tie-in) ------------------------------
add_executable(rdma_token_server rdma_token_server.cc)
target_link_libraries(rdma_token_server PRIVATE rdma_transport)
set_target_properties(rdma_token_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(rdma_token_client rdma_token_client.cc)
target_link_libraries(rdma_token_client PRIVATE rdma_transport)
set_target_properties(rdma_token_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
