include(cmake/peer_grpc.cmake)

# These are required for a folly build
# folly cmake mostly from here: https://stackoverflow.com/questions/56511716/cmake-file-for-a-folly-project
find_package(folly REQUIRED)
find_package(gflags REQUIRED)

configure_file("common/config.h.in" "${PROJECT_BINARY_DIR}/common/config.h")

add_executable(embarlet 
    embarlet/embarlet.cc
    disk_manager/disk_manager.cc 
    disk_manager/disk_manager.h 
    cxl_manager/cxl_manager.cc
    cxl_manager/cxl_manager.h 
    network_manager/network_manager.cc
    network_manager/network_manager.h 
    embarlet/pub_queue.cc 
    embarlet/pub_queue.h 
    embarlet/pub_task.h 
    embarlet/peer.cc
    embarlet/topic_manager.cc
    embarlet/topic_manager.h
    client/publisher.h
)

target_include_directories(embarlet PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}"
)

target_link_libraries(embarlet
    absl::flat_hash_map 
    peer_grpc_proto
    cxxopts::cxxopts
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
    ${FOLLY_LIBRARIES}
)

add_executable(client
    client/client.cc
    client/publisher.cc
    client/subscriber.cc
    client/publisher.h
)

target_link_libraries(client
    peer_grpc_proto
    cxxopts::cxxopts
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOF}
)
