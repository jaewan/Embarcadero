cmake_minimum_required(VERSION 3.10)
project(NetworkEmulationTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Enable threading
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_executable(broker broker.cpp)
target_link_libraries(broker PRIVATE Threads::Threads)

add_executable(client client.cpp)
target_link_libraries(client PRIVATE Threads::Threads)
