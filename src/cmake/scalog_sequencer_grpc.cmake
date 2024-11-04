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
# find_package(gRPC REQUIRED)

# Proto file
get_filename_component(scalog_sequencer_proto "protobuf/scalog_sequencer.proto" ABSOLUTE)
get_filename_component(scalog_sequencer_proto_path "${scalog_sequencer_proto}" PATH)

# Generated sources
set(scalog_sequencer_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/scalog_sequencer.pb.cc")
set(scalog_sequencer_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/scalog_sequencer.pb.h")
set(scalog_sequencer_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/scalog_sequencer.grpc.pb.cc")
set(scalog_sequencer_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/scalog_sequencer.grpc.pb.h")
add_custom_command(
      OUTPUT "${scalog_sequencer_proto_srcs}" "${scalog_sequencer_proto_hdrs}" "${scalog_sequencer_grpc_srcs}" "${scalog_sequencer_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${scalog_sequencer_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${scalog_sequencer_proto}"
      DEPENDS "${scalog_sequencer_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(scalog_sequencer_grpc_proto
  ${scalog_sequencer_grpc_srcs}
  ${scalog_sequencer_grpc_hdrs}
  ${scalog_sequencer_proto_srcs}
  ${scalog_sequencer_proto_hdrs})
target_link_libraries(scalog_sequencer_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})