# Setting the cmake version to 3.5 for abseil compatibility
cmake_minimum_required(VERSION 3.5)

project(Embarcadero VERSION 0.1
	DESCRIPTION "Pubsub with disaggregated memory"
	LANGUAGES CXX)


# Setting the C++ version to 17 for folly compatibility
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "-Wall -O3")
set(CMAKE_CXX_STANDARD_REQUIRED True)

# GRPC library location
list(APPEND CMAKE_PREFIX_PATH "/usr/local/lib/" "/usr/local/lib/cmake/grpc" "/usr/local/bin/")
set(_REFLECTION gRPC::grpc++_reflection)
find_package(gRPC CONFIG REQUIRED)

add_subdirectory(third_party/cxxopts)
add_subdirectory(src)

# Set output to lib/ or bin/ directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
