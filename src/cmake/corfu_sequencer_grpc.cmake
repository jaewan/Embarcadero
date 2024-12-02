find_package(Protobuf REQUIRED)
#find_package(gRPC CONFIG REQUIRED)
#find_package(absl CONFIG REQUIRED)
#find_package(glog CONFIG REQUIRED)

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

# Proto file
get_filename_component(corfu_sequencer_proto "protobuf/corfu_sequencer.proto" ABSOLUTE)
get_filename_component(corfu_sequencer_proto_path "${corfu_sequencer_proto}" PATH)

# Generated sources
set(corfu_sequencer_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_sequencer.pb.cc")
set(corfu_sequencer_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_sequencer.pb.h")
set(corfu_sequencer_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_sequencer.grpc.pb.cc")
set(corfu_sequencer_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_sequencer.grpc.pb.h")

add_custom_command(
      OUTPUT "${corfu_sequencer_proto_srcs}" "${corfu_sequencer_proto_hdrs}" "${corfu_sequencer_grpc_srcs}" "${corfu_sequencer_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${corfu_sequencer_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${corfu_sequencer_proto}"
      DEPENDS "${corfu_sequencer_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(corfu_sequencer_grpc_proto
  ${corfu_sequencer_grpc_srcs}
  ${corfu_sequencer_grpc_hdrs}
  ${corfu_sequencer_proto_srcs}
  ${corfu_sequencer_proto_hdrs})
target_link_libraries(corfu_sequencer_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})
