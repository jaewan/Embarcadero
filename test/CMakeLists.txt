# Test executables
add_executable(cxl_manager_test cxl_manager.cc)
target_link_libraries(cxl_manager_test
    PRIVATE
    glog::glog
    gflags
    pthread
)
target_include_directories(cxl_manager_test
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Add other test executables here as needed
