# Generated gRPC bindings for the client-to-ingress-broker Corfu token proxy.
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)
get_filename_component(corfu_token_proxy_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/corfu_token_proxy.proto" ABSOLUTE)
get_filename_component(corfu_token_proxy_proto_path "${corfu_token_proxy_proto}" PATH)
get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
set(corfu_token_proxy_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_token_proxy.pb.cc")
set(corfu_token_proxy_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_token_proxy.pb.h")
set(corfu_token_proxy_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_token_proxy.grpc.pb.cc")
set(corfu_token_proxy_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_token_proxy.grpc.pb.h")
add_custom_command(
  OUTPUT "${corfu_token_proxy_proto_srcs}" "${corfu_token_proxy_proto_hdrs}" "${corfu_token_proxy_grpc_srcs}" "${corfu_token_proxy_grpc_hdrs}"
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
       -I "${corfu_token_proxy_proto_path}" -I "${protobuf_include_dir}"
       --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}" "${corfu_token_proxy_proto}"
  DEPENDS "${corfu_token_proxy_proto}")
add_library(corfu_token_proxy_grpc_proto ${corfu_token_proxy_grpc_srcs} ${corfu_token_proxy_grpc_hdrs} ${corfu_token_proxy_proto_srcs} ${corfu_token_proxy_proto_hdrs})
target_link_libraries(corfu_token_proxy_grpc_proto grpc++_reflection grpc++ protobuf::libprotobuf)
target_include_directories(corfu_token_proxy_grpc_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
