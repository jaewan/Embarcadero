# gRPC bindings for LazyLog's fault-tolerant sequencing metadata replicas.
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

get_filename_component(lazylog_metadata_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/lazylog_metadata.proto" ABSOLUTE)
get_filename_component(lazylog_metadata_proto_path "${lazylog_metadata_proto}" PATH)

set(lazylog_metadata_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_metadata.pb.cc")
set(lazylog_metadata_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_metadata.pb.h")
set(lazylog_metadata_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_metadata.grpc.pb.cc")
set(lazylog_metadata_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_metadata.grpc.pb.h")

get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

add_custom_command(
    OUTPUT "${lazylog_metadata_proto_srcs}" "${lazylog_metadata_proto_hdrs}" "${lazylog_metadata_grpc_srcs}" "${lazylog_metadata_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${lazylog_metadata_proto_path}"
         -I "${protobuf_include_dir}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${lazylog_metadata_proto}"
    DEPENDS "${lazylog_metadata_proto}"
)

add_library(lazylog_metadata_grpc_proto
    ${lazylog_metadata_grpc_srcs}
    ${lazylog_metadata_grpc_hdrs}
    ${lazylog_metadata_proto_srcs}
    ${lazylog_metadata_proto_hdrs}
)

target_link_libraries(lazylog_metadata_grpc_proto
    grpc++_reflection
    grpc++
    protobuf::libprotobuf
)

target_include_directories(lazylog_metadata_grpc_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}"
)
