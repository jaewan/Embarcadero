include(cmake/heartbeat_grpc.cmake)
include(cmake/scalog_sequencer_grpc.cmake)
include(cmake/lazylog_sequencer_grpc.cmake)
include(cmake/corfu_sequencer_grpc.cmake)
include(cmake/scalog_replication_grpc.cmake)
include(cmake/corfu_replication_grpc.cmake)
include(cmake/session_proto.cmake)

find_package(folly REQUIRED)
find_package(gflags REQUIRED)
find_package(mimalloc REQUIRED)
find_package(glog REQUIRED)
find_package(Threads REQUIRED)
find_package(cxxopts REQUIRED)

# Detect the processor type
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    # Check for Intel specific intrinsics
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
    if(COMPILER_SUPPORTS_MARCH_NATIVE)
        message(STATUS "Enabling -march=native")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
    endif()

    # Detect Intel or AMD processor using lscpu
    execute_process(
        COMMAND lscpu
        COMMAND grep "Vendor ID:"
        COMMAND awk "{print $NF}"
        OUTPUT_VARIABLE CPU_VENDOR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(CPU_VENDOR STREQUAL "GenuineIntel")
        set(__INTEL__ 1)
    elseif(CPU_VENDOR STREQUAL "AuthenticAMD")
        set(__AMD__ 1)
    else()
        message(WARNING "Unknown CPU vendor: ${CPU_VENDOR}")
    endif()
endif()


configure_file("common/config.h.in" "${PROJECT_BINARY_DIR}/common/config.h")

add_executable(embarlet
    embarlet/embarlet.cc
    common/configuration.cc
    common/configuration.h
    disk_manager/disk_manager.cc
    disk_manager/disk_manager.h
    disk_manager/chain_replication.cc
    disk_manager/chain_replication.h
    disk_manager/corfu_replication_manager.cc
    disk_manager/corfu_replication_manager.h
    disk_manager/corfu_replication_client.cc
    disk_manager/corfu_replication_client.h
    disk_manager/scalog_replication_manager.cc
    disk_manager/scalog_replication_manager.h
    disk_manager/scalog_replication_client.cc
    disk_manager/scalog_replication_client.h
    cxl_manager/cxl_manager.cc
    cxl_manager/cxl_manager.h
    cxl_manager/scalog_local_sequencer.cc
    cxl_manager/scalog_local_sequencer.h
    cxl_manager/lazylog_local_sequencer.cc
    cxl_manager/lazylog_local_sequencer.h
    network_manager/network_manager.cc
    network_manager/network_manager.h
    embarlet/heartbeat.cc
    embarlet/topic_manager.cc
    embarlet/topic_manager.h
    embarlet/topic.cc
	embarlet/goi_recovery_thread.cc
    embarlet/topic.h
)


target_include_directories(embarlet PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
  "${PROJECT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(embarlet
    atomic
    numa
    glog::glog
    gflags
    mimalloc
    absl::flat_hash_map
	scalog_sequencer_grpc_proto
    lazylog_sequencer_grpc_proto
	corfu_replication_grpc_proto
    scalog_replication_grpc_proto
	heartbeat_grpc_proto
	session_proto
    cxxopts::cxxopts
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
    ${FOLLY_LIBRARIES}
    yaml-cpp
    Threads::Threads
)

set_target_properties(embarlet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Enable benchmark optimizations for embarlet (commented out due to segfault)
# target_compile_definitions(embarlet PRIVATE BUILDING_ORDER_BENCH)

add_executable(throughput_test
    client/common.cc
    client/common.h
    client/buffer.cc
    client/buffer.h
    client/queue_buffer.cc
    client/queue_buffer.h
    client/publisher.cc
    client/publisher.h
    client/subscriber.cc
    client/subscriber.h
    client/result_writer.cc
    client/result_writer.h
    client/test_utils.cc
    client/test_utils.h
    client/main.cc
    common/configuration.cc
    common/configuration.h
    # [[GLIBC_COMPAT]] Provide __isoc23_strtoul etc. locally so binaries
    # compiled on GLIBC 2.39 (Ubuntu 24.04) run on GLIBC 2.35 (Ubuntu 22.04).
    common/compat_isoc23.cpp
)

target_include_directories(throughput_test PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/client"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(throughput_test
    glog::glog
    gflags
    mimalloc
    cxxopts::cxxopts
    heartbeat_grpc_proto
    session_proto
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

set_target_properties(throughput_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    # [[GLIBC_COMPAT]] Allow binary to find shared libs (e.g. libglog.so.1)
    # from the same directory as itself on remote machines without /usr/local/lib.
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Buffer-only throughput benchmark (no network). Isolates pubQue_/Buffer for serialization investigation.
add_executable(buffer_benchmark
    client/common.cc
    client/common.h
    client/buffer.cc
    client/buffer.h
    client/queue_buffer.cc
    client/queue_buffer.h
    test/publisher_profile_stub.cc
    test/buffer_benchmark.cc
    common/configuration.cc
    common/configuration.h
)

target_include_directories(buffer_benchmark PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/client"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(buffer_benchmark
    glog::glog
    gflags
    mimalloc
    cxxopts::cxxopts
    heartbeat_grpc_proto
    corfu_sequencer_grpc_proto
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
    ${FOLLY_LIBRARIES}
    Threads::Threads
    yaml-cpp
)

set_target_properties(buffer_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(scalog_global_sequencer
    cxl_manager/scalog_global_sequencer.cc
    cxl_manager/scalog_global_sequencer.h
    # Track 03: shared transport-independent global-cut core (S1 extraction). The gRPC
    # baseline and the CXL mailbox port call literally the same ordering code.
    cxl_manager/scalog_global_ordering_core.cc
    cxl_manager/scalog_global_ordering_core.h
    common/configuration.cc
    common/configuration.h
    # [[GLIBC_COMPAT]] Provide __isoc23_* locally for GLIBC 2.35 compatibility.
    common/compat_isoc23.cpp
)

target_include_directories(scalog_global_sequencer PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    # Needed so the S1 core header resolves as "cxl_manager/scalog_global_ordering_core.h".
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(scalog_global_sequencer
    glog::glog
    absl::flat_hash_map
	scalog_sequencer_grpc_proto
    protobuf::libprotobuf
    Threads::Threads
    yaml-cpp
)

set_target_properties(scalog_global_sequencer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)

add_executable(lazylog_global_sequencer
    cxl_manager/lazylog_global_sequencer.cc
    cxl_manager/lazylog_global_sequencer.h
    cxl_manager/lazylog_binding_core.cc
    cxl_manager/lazylog_binding_core.h
    common/configuration.cc
    common/configuration.h
)

target_include_directories(lazylog_global_sequencer PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(lazylog_global_sequencer
    glog::glog
    absl::flat_hash_map
    lazylog_sequencer_grpc_proto
    protobuf::libprotobuf
    Threads::Threads
    yaml-cpp
)

set_target_properties(lazylog_global_sequencer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(corfu_global_sequencer
    cxl_manager/corfu_global_sequencer.cc
    cxl_manager/corfu_sequencer_service.cc
    common/configuration.cc
    common/configuration.h
)

target_include_directories(corfu_global_sequencer PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(corfu_global_sequencer
    glog::glog
    absl::flat_hash_map
	corfu_sequencer_grpc_proto
    grpc++
    protobuf::libprotobuf
    Threads::Threads
    yaml-cpp
)

set_target_properties(corfu_global_sequencer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Deterministic ordering checks for CorfuSequencerImpl (per-client FIFO vs global total_order).
add_executable(corfu_sequencer_fifo_smoke
    cxl_manager/corfu_sequencer_fifo_smoke.cc
    cxl_manager/corfu_sequencer_service.cc
)
target_include_directories(corfu_sequencer_fifo_smoke PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(corfu_sequencer_fifo_smoke
    absl::flat_hash_map
    corfu_sequencer_grpc_proto
    grpc++
    protobuf::libprotobuf
    Threads::Threads
)
set_target_properties(corfu_sequencer_fifo_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME corfu_sequencer_fifo_smoke COMMAND corfu_sequencer_fifo_smoke
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- Corfu CXL mailbox port (Track 03 / E10) ----
# E10 benchmark + FIFO correctness harness. The co-located sequencer poll thread drains
# CorfuTokenRequest records from every up(b) ring, calls CorfuSequencerImpl::AssignToken
# (the SAME ordering code the gRPC baseline uses — pure transport swap per
# docs/baselines/porting_rule.md), and posts CorfuTokenGrant on down(b). corfu_sequencer_service.cc
# is compiled fresh into this executable (separate CorfuSequencerImpl instance, no ODR issue).
# Links cxl_transport (the mailbox library) + the corfu proto (corfu_sequencer_service.cc needs
# its generated headers) + grpc++/protobuf. cxl_transport is defined later in this file; CMake
# resolves the dependency by target name regardless of definition order.
add_executable(corfu_mailbox_bench
    cxl_manager/corfu_mailbox_bench.cc
    cxl_manager/corfu_mailbox_sequencer.cc
    cxl_manager/corfu_sequencer_service.cc
)
target_include_directories(corfu_mailbox_bench PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(corfu_mailbox_bench
    cxl_transport
    corfu_sequencer_grpc_proto
    grpc++
    protobuf::libprotobuf
    glog::glog
    Threads::Threads
)
set_target_properties(corfu_mailbox_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME corfu_mailbox_bench COMMAND corfu_mailbox_bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# ---- Scalog CXL mailbox port (Track 03 / S1-S5) ----
# Correctness + performance harness for the Scalog cut/report path ported onto the CXL
# mailbox. The co-located sequencer poll thread drains ScalogLocalCutMsg records from every
# up(b) ring, feeds ScalogGlobalOrderingCore::AddLocalCut (the SAME accumulation code the
# gRPC baseline uses) and, once the whole replica set has reported, computes the per-broker
# MIN-across-replicas GlobalCut via the core and BroadcastDown()s it to every down(b) — a
# pure transport swap per docs/baselines/porting_rule.md. scalog_global_ordering_core.cc is
# compiled fresh into this executable (separate core instance, no ODR issue). The core and
# sequencer have NO proto dependency (they operate on the fixed PODs in
# scalog_mailbox_messages.h); the scalog proto is linked only to document the message lineage.
add_executable(scalog_mailbox_bench
    cxl_manager/scalog_mailbox_bench.cc
    cxl_manager/scalog_mailbox_sequencer.cc
    cxl_manager/scalog_global_ordering_core.cc
)
target_include_directories(scalog_mailbox_bench PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(scalog_mailbox_bench
    cxl_transport
    scalog_sequencer_grpc_proto
    grpc++
    protobuf::libprotobuf
    glog::glog
    absl::flat_hash_map
    Threads::Threads
    yaml-cpp
)
set_target_properties(scalog_mailbox_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME scalog_mailbox_bench COMMAND scalog_mailbox_bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# ---- LazyLog CXL mailbox port (Track 03 / L1-L5) ----
# Correctness + performance harness for the LazyLog coordinator (binding-round) path ported onto
# the CXL mailbox. The co-located sequencer poll thread drains LazyLogLocalProgressMsg records
# from every up(b) ring, feeds LazyLogBindingCore::AddLocalProgress (the SAME accumulation code the
# gRPC baseline uses) and, once every broker has reported, computes the per-broker binding delta
# via the core and BroadcastDown()s it to every down(b) — a pure transport swap per
# docs/baselines/porting_rule.md. lazylog_binding_core.cc is compiled fresh into this executable
# (separate core instance, no ODR issue). The core and sequencer have NO proto dependency (they
# operate on the fixed PODs in lazylog_mailbox_messages.h); the lazylog proto is linked only to
# document the message lineage. lazylog_global_sequencer.cc is NOT compiled in (it defines main()
# for the standalone gRPC server); the bench uses the extracted LazyLogBindingCore directly.
add_executable(lazylog_mailbox_bench
    cxl_manager/lazylog_mailbox_bench.cc
    cxl_manager/lazylog_mailbox_sequencer.cc
    cxl_manager/lazylog_binding_core.cc
)
target_include_directories(lazylog_mailbox_bench PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(lazylog_mailbox_bench
    cxl_transport
    lazylog_sequencer_grpc_proto
    grpc++
    protobuf::libprotobuf
    glog::glog
    absl::flat_hash_map
    Threads::Threads
    yaml-cpp
)
set_target_properties(lazylog_mailbox_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_test(NAME lazylog_mailbox_bench COMMAND lazylog_mailbox_bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# ---- CXL mailbox transport (Track 03 / W4) ----
# Shared-memory RPC substrate built from our own single-writer rings + clwb/sfence.
# Baseline control paths (Corfu token, Scalog cut/report, LazyLog coordinator) are
# ported onto this per docs/baselines/porting_rule.md (transport swap only).
add_library(cxl_transport
    cxl_transport/cxl_mailbox.cc
    cxl_transport/cxl_mailbox.h
    cxl_transport/cxl_mailbox_ring.h
)

target_include_directories(cxl_transport PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(cxl_transport PUBLIC
    glog::glog
    Threads::Threads
    rt
)

# Smoke test for the mailbox transport. The default path uses anonymous MAP_SHARED
# across fork() (no /dev/shm, no testbed lock). `--shm` adds a POSIX-shm variant to run
# under the flock.
add_executable(cxl_mailbox_smoke
    cxl_transport/cxl_mailbox_smoke.cc
)

target_link_libraries(cxl_mailbox_smoke
    cxl_transport
    glog::glog
    Threads::Threads
)

set_target_properties(cxl_mailbox_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME cxl_mailbox_smoke COMMAND cxl_mailbox_smoke
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Configuration test executable
add_executable(config_test
    common/config_example.cc
    common/configuration.cc
    common/configuration.h
)

target_include_directories(config_test PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
)

target_link_libraries(config_test
    glog::glog
    yaml-cpp
    Threads::Threads
)

set_target_properties(config_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
