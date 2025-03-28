# Use gRPC's targets for protoc and the plugin
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

# Proto file path
get_filename_component(corfu_replication_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/corfu_replication.proto" ABSOLUTE)
get_filename_component(corfu_replication_proto_path "${corfu_replication_proto}" PATH)

# Generated sources
set(corfu_replication_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_replication.pb.cc")
set(corfu_replication_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_replication.pb.h")
set(corfu_replication_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_replication.grpc.pb.cc")
set(corfu_replication_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_replication.grpc.pb.h")

# Get the path to protobuf's well_known_protos
get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

# Generate the code
add_custom_command(
    OUTPUT "${corfu_replication_proto_srcs}" "${corfu_replication_proto_hdrs}" "${corfu_replication_grpc_srcs}" "${corfu_replication_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${corfu_replication_proto_path}"
         -I "${protobuf_include_dir}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${corfu_replication_proto}"
    DEPENDS "${corfu_replication_proto}"
)

# Create a library target
add_library(corfu_replication_grpc_proto
    ${corfu_replication_grpc_srcs}
    ${corfu_replication_grpc_hdrs}
    ${corfu_replication_proto_srcs}
    ${corfu_replication_proto_hdrs}
)

# Link against gRPC and Protobuf
target_link_libraries(corfu_replication_grpc_proto
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
)

# Use target_include_directories instead of include_directories
target_include_directories(corfu_replication_grpc_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}"
)
