# Use gRPC's targets for protoc and the plugin
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

get_filename_component(lazylog_sequencer_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/lazylog_sequencer.proto" ABSOLUTE)
get_filename_component(lazylog_sequencer_proto_path "${lazylog_sequencer_proto}" PATH)

set(lazylog_sequencer_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_sequencer.pb.cc")
set(lazylog_sequencer_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_sequencer.pb.h")
set(lazylog_sequencer_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_sequencer.grpc.pb.cc")
set(lazylog_sequencer_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/lazylog_sequencer.grpc.pb.h")

get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

add_custom_command(
    OUTPUT "${lazylog_sequencer_proto_srcs}" "${lazylog_sequencer_proto_hdrs}" "${lazylog_sequencer_grpc_srcs}" "${lazylog_sequencer_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${lazylog_sequencer_proto_path}"
         -I "${protobuf_include_dir}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${lazylog_sequencer_proto}"
    DEPENDS "${lazylog_sequencer_proto}"
)

add_library(lazylog_sequencer_grpc_proto
    ${lazylog_sequencer_grpc_srcs}
    ${lazylog_sequencer_grpc_hdrs}
    ${lazylog_sequencer_proto_srcs}
    ${lazylog_sequencer_proto_hdrs}
)

target_link_libraries(lazylog_sequencer_grpc_proto
    grpc++_reflection
    grpc++
    libprotobuf
)

target_include_directories(lazylog_sequencer_grpc_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}"
)
