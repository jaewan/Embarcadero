# Use gRPC's targets for protoc and the plugin
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

# Proto file path
get_filename_component(scalog_replication_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/scalog_replication.proto" ABSOLUTE)
get_filename_component(scalog_replication_proto_path "${scalog_replication_proto}" PATH)

# Generated sources
set(scalog_replication_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/scalog_replication.pb.cc")
set(scalog_replication_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/scalog_replication.pb.h")
set(scalog_replication_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/scalog_replication.grpc.pb.cc")
set(scalog_replication_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/scalog_replication.grpc.pb.h")

# Get the path to protobuf's well_known_protos
get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

# Generate the code
add_custom_command(
    OUTPUT "${scalog_replication_proto_srcs}" "${scalog_replication_proto_hdrs}" "${scalog_replication_grpc_srcs}" "${scalog_replication_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${scalog_replication_proto_path}"
         -I "${protobuf_include_dir}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${scalog_replication_proto}"
    DEPENDS "${scalog_replication_proto}"
)

# Create a library target
add_library(scalog_replication_grpc_proto
    ${scalog_replication_grpc_srcs}
    ${scalog_replication_grpc_hdrs}
    ${scalog_replication_proto_srcs}
    ${scalog_replication_proto_hdrs}
)

# Link against gRPC and Protobuf
target_link_libraries(scalog_replication_grpc_proto
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
)

# Use target_include_directories instead of include_directories
target_include_directories(scalog_replication_grpc_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}"
)