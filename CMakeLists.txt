cmake_minimum_required(VERSION 3.20)

# Suppress deprecation warnings from bundled dependencies
set(CMAKE_WARN_DEPRECATED OFF)

project(Embarcadero VERSION 0.1
        DESCRIPTION "Pubsub with disaggregated memory"
        LANGUAGES CXX)

# Load custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_MESSAGE LAZY)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(cmake/EmbarcaderoBuildOptions.cmake)
option(EMBARCADERO_BUILD_BROKER "Build the broker (includes baseline runtime adapters)" ON)
option(EMBARCADERO_BUILD_BASELINE_TOOLS "Build standalone comparison baseline tools" ON)
option(EMBARCADERO_CLIENT_BASELINES "Include Corfu RPC support in the client" ON)
option(EMBARCADERO_BUILD_BENCHMARKS "Build component and KV benchmarks" ON)

include(FetchContent)
set(ABSL_ENABLE_INSTALL OFF) # Do NOT install Abseil
set(ABSL_PROPAGATE_CXX_STD ON) # Suppress Abseil warning about C++ standard propagation
set(gRPC_INSTALL OFF)       # Do NOT install gRPC
set(protobuf_INSTALL OFF)  # Do NOT install Protobuf

# Worried about dynamic linking of these libraries, may not be needed
set(BUILD_SHARED_LIBS OFF)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "Don't build gRPC tests." FORCE)

FetchContent_Declare(
  gRPC
  GIT_REPOSITORY https://github.com/grpc/grpc
  GIT_TAG        12161ee3aa7c216741cd7c406573abc0df1d0926 # v1.55.1; immutable revision
)
set(FETCHCONTENT_QUIET OFF) # Consider turning this on after initial setup
FetchContent_MakeAvailable(gRPC)
 
# Find packages (installed by setup scripts)
find_package(Mimalloc REQUIRED)
find_package(gflags REQUIRED)
find_package(glog REQUIRED)
find_package(folly REQUIRED)

# Find yaml-cpp for configuration management
find_package(yaml-cpp REQUIRED)

# Enable CTest (required for add_test() in test/ and E2E scripts)
include(CTest)

# Option to enable latency statistics collection (compile-time removal for performance)
option(COLLECT_LATENCY_STATS "Enable latency statistics collection (adds overhead)" OFF)
if(COLLECT_LATENCY_STATS)
    add_compile_definitions(COLLECT_LATENCY_STATS)
    message(STATUS "Latency statistics collection enabled")
else()
    message(STATUS "Latency statistics collection disabled (optimal performance)")
endif()

# Add subdirectories
add_subdirectory(src)
if(EMBARCADERO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
# Apply project options after target creation; bundled dependencies are excluded.
embarcadero_apply_options("${CMAKE_CURRENT_SOURCE_DIR}/src")
if(EMBARCADERO_BUILD_BENCHMARKS)
    embarcadero_apply_options("${CMAKE_CURRENT_SOURCE_DIR}/benchmarks")
endif()
if(BUILD_TESTING)
    embarcadero_apply_options("${CMAKE_CURRENT_SOURCE_DIR}/test")
    embarcadero_write_test_inventory()
endif()
