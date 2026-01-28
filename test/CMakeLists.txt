# Embarcadero Test Suite

# End-to-End Tests (Shell scripts)
# These run actual broker clusters and validate system behavior

# Add E2E tests as custom targets
add_custom_target(test_e2e
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_all.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running end-to-end tests"
    VERBATIM
)

# Individual E2E tests
add_test(
    NAME e2e_basic_publish
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_basic_publish.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(
    NAME e2e_explicit_replication_order5_ack2
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_explicit_replication_order5_ack2.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Set test properties
set_tests_properties(e2e_basic_publish PROPERTIES
    TIMEOUT 300  # 5 minutes max
    LABELS "e2e;basic;smoke"
)

set_tests_properties(e2e_explicit_replication_order5_ack2 PROPERTIES
    TIMEOUT 600  # 10 minutes max (replication may take longer)
    LABELS "e2e;replication;order5;ack2"
)

# Unit tests
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GTEST gtest)
    if(GTEST_FOUND)
        message(STATUS "GTest found via pkg-config, enabling unit tests")
        
        # BlogMessageHeader validation test
        add_executable(blog_header_validation_test
            embarlet/blog_header_validation_test.cc
        )
        target_include_directories(blog_header_validation_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(blog_header_validation_test PRIVATE ${GTEST_CFLAGS_OTHER})
        # Link against required libraries
        # Link GTest main library explicitly
        find_library(GTEST_MAIN_LIB gtest_main PATHS /usr/lib /usr/lib/x86_64-linux-gnu)
        target_link_libraries(blog_header_validation_test PRIVATE 
            ${GTEST_LIBRARIES} 
            ${GTEST_MAIN_LIB}
            absl::flat_hash_map
            heartbeat_grpc_proto
            protobuf::libprotobuf
            pthread
        )
        
        add_test(NAME unit_blog_header_validation COMMAND blog_header_validation_test)
        set_tests_properties(unit_blog_header_validation PROPERTIES
            TIMEOUT 60
            LABELS "unit;blog_header;validation"
        )
    else()
        message(WARNING "GTest not found via pkg-config, unit tests disabled")
    endif()
else()
    message(WARNING "pkg-config not found, unit tests disabled")
endif()

# Test output directory
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_output)

# Add test dependencies
add_dependencies(test_e2e embarlet throughput_test)
