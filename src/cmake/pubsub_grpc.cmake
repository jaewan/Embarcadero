# Generate grpc stubs for pubsub class
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
#find_package(gRPC REQUIRED)

# Proto file
get_filename_component(pubsub_proto "protobuf/pubsub.proto" ABSOLUTE)
get_filename_component(pubsub_proto_path "${pubsub_proto}" PATH)

# Generated sources
set(pubsub_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/pubsub.pb.cc")
set(pubsub_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/pubsub.pb.h")
set(pubsub_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/pubsub.grpc.pb.cc")
set(pubsub_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/pubsub.grpc.pb.h")
add_custom_command(
      OUTPUT "${pubsub_proto_srcs}" "${pubsub_proto_hdrs}" "${pubsub_grpc_srcs}" "${pubsub_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${pubsub_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${pubsub_proto}"
      DEPENDS "${pubsub_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(pubsub_grpc_proto
  ${pubsub_grpc_srcs}
  ${pubsub_grpc_hdrs}
  ${pubsub_proto_srcs}
  ${pubsub_proto_hdrs})
target_link_libraries(pubsub_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})