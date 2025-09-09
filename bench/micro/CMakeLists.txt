cmake_minimum_required(VERSION 3.16)

find_package(cxxopts REQUIRED)

add_executable(order_micro_bench
    order_micro_main.cc
    cxl_shm_wrapper.cc
    ../../src/embarlet/message_ordering.cc
    ../../src/common/configuration.cc
)

target_include_directories(order_micro_bench PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/../../src"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
)

target_link_libraries(order_micro_bench
    glog::glog
    gflags
    mimalloc
    cxxopts::cxxopts
    absl::flat_hash_map
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
    numa
    Threads::Threads
    yaml-cpp
)

set_target_properties(order_micro_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

target_compile_definitions(order_micro_bench PRIVATE BUILDING_ORDER_BENCH)
