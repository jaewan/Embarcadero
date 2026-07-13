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
set(CMAKE_CXX_FLAGS "-Wall -O3")
set(CMAKE_INSTALL_MESSAGE LAZY)
set(CMAKE_PARALLEL_LEVEL 16)

# performance_utils.h calls _mm_clflushopt on x86.  Check and enable the
# precise compiler feature rather than assuming -march=native was accepted;
# the latter can be absent from generated target flags and leaves every CXL
# translation unit uncompilable.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
	include(CheckCXXCompilerFlag)
	check_cxx_compiler_flag("-mclflushopt" COMPILER_SUPPORTS_CLFLUSHOPT)
	if(NOT COMPILER_SUPPORTS_CLFLUSHOPT)
		message(FATAL_ERROR "This x86 build requires compiler support for -mclflushopt")
	endif()
	add_compile_options("-mclflushopt")
endif()

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
  GIT_TAG        v1.55.1
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
enable_testing()

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
add_subdirectory(benchmarks)   # KV benchmark + component micro-benchmarks
add_subdirectory(test)
