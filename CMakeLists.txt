cmake_minimum_required(VERSION 3.16)

project(Embarcadero VERSION 0.1
					DESCRIPTION "Pubsub with disaggregated memory"
					LANGUAGES CXX)

# Setting the C++ version to 14 for abseil compatibility
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "-Wall")
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(src)
