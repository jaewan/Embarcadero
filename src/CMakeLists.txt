include(cmake/heartbeat_grpc.cmake)
include(cmake/scalog_sequencer_grpc.cmake)
include(cmake/corfu_sequencer_grpc.cmake)

# These are required for a folly build
# folly cmake mostly from pehere: https://stackoverflow.com/questions/56511716/cmake-file-for-a-folly-project
find_package(folly REQUIRED)
find_package(gflags REQUIRED)
find_package(mimalloc REQUIRED)

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

# Include the Protobuf and gRPC include directories passed from the parent CMakeLists.txt
include_directories(${PROTOBUF_INCLUDE_DIRS})

# Directories for your .proto files
set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")

# List of .proto files
set(PROTO_FILES
    "${PROTO_SRC_DIR}/heartbeat.proto"
)

set(CORFU_CLIENT_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/client/corfu_client.h" 
)

add_executable(embarlet 
    embarlet/embarlet.cc
    disk_manager/disk_manager.cc 
    disk_manager/disk_manager.h 
    cxl_manager/cxl_manager.cc
    cxl_manager/cxl_manager.h 
    network_manager/network_manager.cc
    network_manager/network_manager.h 
    embarlet/heartbeat.cc
    embarlet/topic_manager.cc
    embarlet/topic_manager.h
)
add_custom_command(
  OUTPUT "${heartbeat_proto_srcs}" "${heartbeat_proto_hdrs}" "${heartbeat_grpc_srcs}" "${heartbeat_grpc_hdrs}"
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
    --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${heartbeat_proto_path}"
    -I "${PROTOBUF_INCLUDE_DIRS}"  # Add this line
    --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
    "${heartbeat_proto}"
  DEPENDS "${heartbeat_proto}"
)

target_include_directories(embarlet PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
  "${PROJECT_BINARY_DIR}"
)

target_link_libraries(embarlet
	numa
	glog
	gflags
	mimalloc
    absl::flat_hash_map 
    scalog_sequencer_grpc_proto
	heartbeat_grpc_proto
    cxxopts::cxxopts
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
    ${FOLLY_LIBRARIES}
)

add_executable(throughput_test
    client/throughput_test.cc
)

target_include_directories(throughput_test PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/client"
)

target_link_libraries(throughput_test 
	glog
	gflags
	mimalloc
    cxxopts::cxxopts
    heartbeat_grpc_proto
	corfu_sequencer_grpc_proto
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOF}
    ${FOLLY_LIBRARIES}
)

add_executable(scalog_global_sequencer
    cxl_manager/scalog_global_sequencer.cc
    cxl_manager/scalog_global_sequencer.h
)

target_include_directories(scalog_global_sequencer PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
)

target_link_libraries(scalog_global_sequencer 
    glog
    absl::flat_hash_map
    scalog_sequencer_grpc_proto
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOF}
)

add_executable(corfu_global_sequencer
    cxl_manager/corfu_global_sequencer.cc
)

target_include_directories(corfu_global_sequencer PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
)

target_link_libraries(corfu_global_sequencer 
    glog
    absl::flat_hash_map
    corfu_sequencer_grpc_proto
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOF}
)
