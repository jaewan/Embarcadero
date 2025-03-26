# Third party dependencies configuration

# cxxopts
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cxxopts/CMakeLists.txt")
    add_subdirectory(cxxopts)
else()
    message(FATAL_ERROR "cxxopts not found in third_party directory")
endif()
