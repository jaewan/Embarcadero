cmake_minimum_required(VERSION 3.16)

project(Embarcadero VERSION 0.1
        DESCRIPTION "Pubsub with disaggregated memory"
        LANGUAGES CXX)

# Load custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-Wall -O3")
set(CMAKE_INSTALL_MESSAGE LAZY)
set(CMAKE_PARALLEL_LEVEL 16)

include(FetchContent)
set(ABSL_ENABLE_INSTALL OFF) # Do NOT install Abseil
set(gRPC_INSTALL OFF)       # Do NOT install gRPC
set(protobuf_INSTALL OFF)  # Do NOT install Protobuf

# Worried about dynamic linking of these libraries, may not be needed
set(BUILD_SHARED_LIBS OFF)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "Don't build gRPC tests." FORCE)

FetchContent_Declare(
  gRPC
  GIT_REPOSITORY https://github.com/grpc/grpc
  GIT_TAG        v1.55.1
)
set(FETCHCONTENT_QUIET OFF) # Consider turning this on after initial setup
FetchContent_MakeAvailable(gRPC)
 
# Find packages (installed by setup scripts)
find_package(Mimalloc REQUIRED)
find_package(gflags REQUIRED)
find_package(glog REQUIRED)
find_package(folly REQUIRED)

# Setup source
add_subdirectory(src)
