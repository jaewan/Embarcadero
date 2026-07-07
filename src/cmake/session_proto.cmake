# Use gRPC's protobuf target for protoc. This schema is message-only; no service
# stubs are generated.
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)

get_filename_component(session_proto "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/session.proto" ABSOLUTE)
get_filename_component(session_proto_path "${session_proto}" PATH)

set(session_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/session.pb.cc")
set(session_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/session.pb.h")

get_target_property(protobuf_include_dir protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

add_custom_command(
	OUTPUT "${session_proto_srcs}" "${session_proto_hdrs}"
	COMMAND ${_PROTOBUF_PROTOC}
	ARGS --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
	     -I "${session_proto_path}"
	     -I "${protobuf_include_dir}"
	     "${session_proto}"
	DEPENDS "${session_proto}"
)

add_library(session_proto
	${session_proto_srcs}
	${session_proto_hdrs}
)

target_link_libraries(session_proto
	protobuf::libprotobuf
)

target_include_directories(session_proto
	PUBLIC "${CMAKE_CURRENT_BINARY_DIR}"
)
