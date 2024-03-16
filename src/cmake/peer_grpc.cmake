# Generate grpc stubs for peer class
set(_PROTOBUF_LIBPROTOBUF libprotobuf)
set(_REFLECTION grpc++_reflection)
set(_ORCA_SERVICE grpcpp_orca_service)
if(CMAKE_CROSSCOMPILING)
  find_program(_PROTOBUF_PROTOC protoc)
else()
  set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
endif()
set(_GRPC_GRPCPP grpc++)
if(CMAKE_CROSSCOMPILING)
  find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
else()
  set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)
endif()

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# Proto file
get_filename_component(peer_proto "protobuf/peer.proto" ABSOLUTE)
get_filename_component(peer_proto_path "${peer_proto}" PATH)

# Generated sources
set(peer_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/peer.pb.cc")
set(peer_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/peer.pb.h")
set(peer_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/peer.grpc.pb.cc")
set(peer_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/peer.grpc.pb.h")
add_custom_command(
      OUTPUT "${peer_proto_srcs}" "${peer_proto_hdrs}" "${peer_grpc_srcs}" "${peer_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${peer_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${peer_proto}"
      DEPENDS "${peer_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(peer_grpc_proto
  ${peer_grpc_srcs}
  ${peer_grpc_hdrs}
  ${peer_proto_srcs}
  ${peer_proto_hdrs})
target_link_libraries(peer_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})