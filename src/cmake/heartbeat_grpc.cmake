# Generate grpc stubs for heartbeat class
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
get_filename_component(heartbeat_proto "protobuf/heartbeat.proto" ABSOLUTE)
get_filename_component(heartbeat_proto_path "${heartbeat_proto}" PATH)

# Generated sources
set(heartbeat_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.pb.cc")
set(heartbeat_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.pb.h")
set(heartbeat_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.grpc.pb.cc")
set(heartbeat_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/heartbeat.grpc.pb.h")
add_custom_command(
      OUTPUT "${heartbeat_proto_srcs}" "${heartbeat_proto_hdrs}" "${heartbeat_grpc_srcs}" "${heartbeat_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${heartbeat_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${heartbeat_proto}"
      DEPENDS "${heartbeat_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(heartbeat_grpc_proto
  ${heartbeat_grpc_srcs}
  ${heartbeat_grpc_hdrs}
  ${heartbeat_proto_srcs}
  ${heartbeat_proto_hdrs})
target_link_libraries(heartbeat_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})
