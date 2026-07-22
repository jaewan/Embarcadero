cmake_minimum_required(VERSION 3.16)

find_package(cxxopts REQUIRED)

set(KV_CLIENT_SOURCES
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
    # [[GLIBC_COMPAT]] Provide __isoc23_strtoul etc. locally so binaries
    # compiled on GLIBC 2.39 (Ubuntu 24.04) run on GLIBC 2.35 (Ubuntu 22.04).
    # Mirrors src/CMakeLists.txt's throughput_test target.
    ../../src/common/compat_isoc23.cpp
)

set(KV_INCLUDE_DIRS
    "${CMAKE_CURRENT_BINARY_DIR}/../../src"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/client"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(KV_LINK_LIBS
    glog::glog
    gflags
    mimalloc
    cxxopts::cxxopts
    heartbeat_grpc_proto
    session_proto
    corfu_sequencer_grpc_proto
    corfu_token_proxy_grpc_proto
    corfu_replication_grpc_proto
    scalog_replication_grpc_proto
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
    ${FOLLY_LIBRARIES}
    Threads::Threads
    yaml-cpp
)

# Legacy batch benchmark
add_executable(kv_store_bench
    main.cc
    ${KV_CLIENT_SOURCES}
)
target_include_directories(kv_store_bench PUBLIC ${KV_INCLUDE_DIRS})
target_link_libraries(kv_store_bench ${KV_LINK_LIBS})
set_target_properties(kv_store_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Shared-log KV benchmark (throughput + latency)
add_executable(kv_ycsb_bench
    kv_bench_main.cc
    ${KV_CLIENT_SOURCES}
)
target_include_directories(kv_ycsb_bench PUBLIC ${KV_INCLUDE_DIRS})
target_link_libraries(kv_ycsb_bench ${KV_LINK_LIBS})
set_target_properties(kv_ycsb_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    # [[GLIBC_COMPAT]] Allow binary to find shared libs (e.g. libglog.so.1)
    # from the same directory as itself on remote machines without /usr/local/lib.
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)
