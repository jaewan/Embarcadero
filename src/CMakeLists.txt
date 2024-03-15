configure_file(config.h.in config.h)

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

set(_REFLECTION gRPC::grpc++_reflection)

# Find gRPC installation
# Looks for gRPCConfig.cmake file installed by gRPC's cmake installation.
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION}")

set(_GRPC_GRPCPP gRPC::grpc++)
if(CMAKE_CROSSCOMPILING)
  find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
else()
  set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

add_executable(embarlet embarlet/embarlet.cc embarlet/publisher_queue.cc disk_manager/disk_manager.cc network_manager/network_manager.cc network_manager/network_manager.h config.h)


target_link_libraries(embarlet ${FOLLY_LIBRARIES})
target_link_libraries(embarlet PRIVATE ${_REFLECTION} ${_GRPC_GRPCPP})

target_include_directories(embarlet PUBLIC
							"${CMAKE_CURRENT_BINARY_DIR}"
						   )

target_link_libraries(embarlet absl::strings)