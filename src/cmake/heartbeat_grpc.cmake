# Use gRPC's targets for protoc and the plugin
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

# Proto file
get_filename_component(heartbeat_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/heartbeat.proto" ABSOLUTE)
get_filename_component(heartbeat_proto_path "${heartbeat_proto}" PATH)

# Generated sources
set(heartbeat_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.pb.cc")
set(heartbeat_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.pb.h")
set(heartbeat_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.grpc.pb.cc")
set(heartbeat_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.grpc.pb.h")

# Get the path to protobuf's well_known_protos
get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

# Generate the code
add_custom_command(
    OUTPUT "${heartbeat_proto_srcs}" "${heartbeat_proto_hdrs}" "${heartbeat_grpc_srcs}" "${heartbeat_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${heartbeat_proto_path}"
         -I "${protobuf_include_dir}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${heartbeat_proto}"
    DEPENDS "${heartbeat_proto}"
)

# Create a library target
add_library(heartbeat_grpc_proto
    ${heartbeat_grpc_srcs}
    ${heartbeat_grpc_hdrs}
    ${heartbeat_proto_srcs}
    ${heartbeat_proto_hdrs}
)

# Link against gRPC and Protobuf
target_link_libraries(heartbeat_grpc_proto
    grpc++_reflection
    grpc++
    libprotobuf
)

# Use target_include_directories instead of include_directories
target_include_directories(heartbeat_grpc_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}"
)
