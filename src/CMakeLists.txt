include(cmake/peer_grpc.cmake)

configure_file(config.h.in config.h)

add_executable(embarlet 
	embarlet/embarlet.cc 
    embarlet/peer.cc
	disk_manager/disk_manager.cc
	config.h
)  

target_include_directories(embarlet PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(embarlet
    absl::flat_hash_map 
    peer_grpc_proto
    cxxopts::cxxopts
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
)