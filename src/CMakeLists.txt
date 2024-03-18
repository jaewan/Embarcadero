include(cmake/peer_grpc.cmake)

configure_file(common/config.h.in common/config.h)

# These are required for a folly build
# folly cmake mostly from here: https://stackoverflow.com/questions/56511716/cmake-file-for-a-folly-project
find_package(folly REQUIRED)
find_package(gflags REQUIRED)

set_and_check(FOLLY_INCLUDE_DIR /usr/local/include/folly)
set_and_check(FOLLY_CMAKE_DIR /usr/local/lib/cmake/folly)
if (NOT TARGET Folly::folly)
  include("${FOLLY_CMAKE_DIR}/folly-targets.cmake")
endif()

set(FOLLY_LIBRARIES Folly::folly)

if (NOT folly_FIND_QUIETLY)
  message(STATUS "Found folly: ${PACKAGE_PREFIX_DIR}")
endif()

add_executable(embarlet 
    embarlet/embarlet.cc
    disk_manager/disk_manager.cc 
    embarlet/pub_queue.cc 
    embarlet/pub_queue.h 
    embarlet/pub_task.h 
    embarlet/peer.cc
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
target_link_libraries(embarlet ${FOLLY_LIBRARIES})
