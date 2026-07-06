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
