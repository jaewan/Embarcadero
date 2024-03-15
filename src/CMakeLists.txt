add_library(proto-objects OBJECT "${CMAKE_CURRENT_LIST_DIR}/protobuf/peer.proto")

target_link_libraries(proto-objects PUBLIC protobuf::libprotobuf gRPC::grpc++_unsecure)

set(PROTO_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PROTO_IMPORT_DIRS "${CMAKE_CURRENT_LIST_DIR}/protobuf")

protobuf_generate(
    TARGET proto-objects
    OUT_VAR PROTO_GENERATED_FILES
    IMPORT_DIRS ${PROTO_IMPORT_DIRS}
    PROTOC_OUT_DIR "${PROTO_BINARY_DIR}")
set_source_files_properties(${PROTO_GENERATED_FILES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION on)

protobuf_generate(
    TARGET proto-objects
    OUT_VAR PROTO_GENERATED_FILES
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    # PLUGIN_OPTIONS "generate_mock_code=true"
    IMPORT_DIRS ${PROTO_IMPORT_DIRS}
    PROTOC_OUT_DIR "${PROTO_BINARY_DIR}")
set_source_files_properties(${PROTO_GENERATED_FILES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION on)

target_include_directories(proto-objects PUBLIC "$<BUILD_INTERFACE:${PROTO_BINARY_DIR}>")

configure_file(config.h.in config.h)

add_executable(embarlet 
	embarlet/embarlet.cc 
	disk_manager/disk_manager.cc
	config.h
)  

target_include_directories(embarlet PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(embarlet
    absl::flat_hash_map 
)

# # Proto file
# get_filename_component(peer_proto "protobuf/peer.proto" ABSOLUTE)
# get_filename_component(peer_proto_path "${peer_proto}" PATH)

# # Generated sources
# set(peer_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/peer.pb.cc")
# set(peer_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/peer.pb.h")
# set(peer_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/peer.grpc.pb.cc")
# set(peer_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/peer.grpc.pb.h")
# add_custom_command(
#       OUTPUT "${peer_proto_srcs}" "${peer_proto_hdrs}" "${peer_grpc_srcs}" "${peer_grpc_hdrs}"
#       COMMAND ${_PROTOBUF_PROTOC}
#       ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
#         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
#         -I "${peer_proto_path}"
#         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
#         "${peer_proto}"
#       DEPENDS "${peer_proto}")

# # Include generated *.pb.h files
# include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# # hw_grpc_proto
# add_library(peer_grpc_proto
#   ${peer_proto_srcs}
#   ${peer_proto_hdrs}
#   ${peer_grpc_srcs}
#   ${peer_grpc_hdrs})
# target_link_libraries(peer_grpc_proto
#   ${_REFLECTION}
#   ${_GRPC_GRPCPP}
#   ${_PROTOBUF_LIBPROTOBUF})

# add_library(proto-objects OBJECT "${CMAKE_CURRENT_LIST_DIR}/protobuf/peer.proto")
# target_link_libraries(proto-objects PUBLIC protobuf::libprotobuf gRPC::grpc++_unsecure)

# set(PROTO_IMPORT_DIRS "${CMAKE_CURRENT_LIST_DIR}/protobuf")
# set(PROTO_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

# protobuf_generate(
#     TARGET proto-objects
#     OUT_VAR PROTO_GENERATED_FILES
#     IMPORT_DIRS ${PROTO_IMPORT_DIRS}
#     PROTOC_OUT_DIR "${PROTO_BINARY_DIR}")
# set_source_files_properties(${PROTO_GENERATED_FILES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION on)

# protobuf_generate(
#     TARGET proto-objects
#     OUT_VAR PROTO_GENERATED_FILES
#     LANGUAGE grpc
#     GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
#     PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
#     # PLUGIN_OPTIONS "generate_mock_code=true"
#     IMPORT_DIRS ${PROTO_IMPORT_DIRS}
#     PROTOC_OUT_DIR "${PROTO_BINARY_DIR}")
# set_source_files_properties(${PROTO_GENERATED_FILES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION on)

# target_include_directories(proto-objects PUBLIC "$<BUILD_INTERFACE:${PROTO_BINARY_DIR}>")
