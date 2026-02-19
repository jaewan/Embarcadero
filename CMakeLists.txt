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

# Enable x86-64 CLFLUSHOPT for CXL flush (performance_utils.h) — required by src and bench
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
	include(CheckCXXCompilerFlag)
	check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
	if(COMPILER_SUPPORTS_MARCH_NATIVE)
		message(STATUS "Enabling -march=native for CXL intrinsics")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
	endif()
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
add_subdirectory(bench)
add_subdirectory(test)

# Benchmarks
option(BUILD_BENCHMARKS "Build performance benchmarks" ON)
if(BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(performance_test benchmark/performance_test.cc)
        target_link_libraries(performance_test
            PRIVATE
            benchmark::benchmark
            absl::hash
            absl::synchronization
            pthread
        )
        target_include_directories(performance_test PRIVATE ${CMAKE_SOURCE_DIR})
        
        # Install performance_test to bin directory
        set_target_properties(performance_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    else()
        message(WARNING "Google Benchmark not found, skipping performance tests")
    endif()
endif()
