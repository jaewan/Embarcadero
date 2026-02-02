cmake_minimum_required(VERSION 3.16)

find_package(cxxopts REQUIRED)

add_executable(kv_store_bench
    main.cc
    distributed_kv_store.cc
    distributed_kv_store.h
    ../../src/client/publisher.cc
    ../../src/client/publisher.h
    ../../src/client/queue_buffer.cc
    ../../src/client/queue_buffer.h
    ../../src/client/subscriber.cc
    ../../src/client/subscriber.h
    ../../src/client/common.cc
    ../../src/client/common.h
    ../../src/client/buffer.cc
    ../../src/client/buffer.h
    ../../src/common/configuration.cc
    ../../src/common/configuration.h
)

target_include_directories(kv_store_bench PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/../../src"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/client"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(kv_store_bench
    glog::glog
    gflags
    mimalloc
    cxxopts::cxxopts
    heartbeat_grpc_proto
    corfu_sequencer_grpc_proto
    corfu_replication_grpc_proto
    scalog_replication_grpc_proto
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
    ${FOLLY_LIBRARIES}
    Threads::Threads
    yaml-cpp
)

set_target_properties(kv_store_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
