cmake_minimum_required(VERSION 3.16)

project(Embarcadero VERSION 0.1
	DESCRIPTION "Pubsub with disaggregated memory"
	LANGUAGES CXX)

# Setting the cmake version to 3.5 for abseil compatibility
cmake_minimum_required(VERSION 3.5)

# Setting the C++ version to 17 for folly compatibility
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "-Wall -O3")
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(third_party/grpc)
add_subdirectory(third_party/cxxopts)
add_subdirectory(src)
add_subdirectory(abseil-cpp)

# Set output to lib/ or bin/ directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
