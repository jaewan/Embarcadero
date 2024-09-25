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
#find_package(gRPC REQUIRED)

# Proto file
get_filename_component(corfu_validator_proto "protobuf/corfu_validator.proto" ABSOLUTE)
get_filename_component(corfu_validator_proto_path "${corfu_validator_proto}" PATH)

# Generated sources
set(corfu_validator_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_validator.pb.cc")
set(corfu_validator_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_validator.pb.h")
set(corfu_validator_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/corfu_validator.grpc.pb.cc")
set(corfu_validator_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/corfu_validator.grpc.pb.h")
add_custom_command(
      OUTPUT "${corfu_validator_proto_srcs}" "${corfu_validator_proto_hdrs}" "${corfu_validator_grpc_srcs}" "${corfu_validator_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${corfu_validator_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${corfu_validator_proto}"
      DEPENDS "${corfu_validator_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(corfu_validator_grpc_proto
  ${corfu_validator_grpc_srcs}
  ${corfu_validator_grpc_hdrs}
  ${corfu_validator_proto_srcs}
  ${corfu_validator_proto_hdrs})
target_link_libraries(corfu_validator_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})
