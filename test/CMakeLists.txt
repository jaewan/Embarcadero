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

# Set test properties
set_tests_properties(e2e_basic_publish PROPERTIES
    TIMEOUT 300  # 5 minutes max
    LABELS "e2e;basic;smoke"
)

# TODO: Unit tests (currently disabled, see archive/)
# Uncomment when ready to re-enable unit tests
#
# find_package(GTest REQUIRED)
#
# add_executable(buffer_manager_test
#     embarlet/buffer_manager_test.cc
# )
# target_link_libraries(buffer_manager_test
#     PRIVATE
#     GTest::GTest
#     GTest::Main
#     gmock
#     embarlet_lib
# )
# add_test(NAME unit_buffer_manager COMMAND buffer_manager_test)

# Test output directory
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_output)

# Add test dependencies
add_dependencies(test_e2e embarlet throughput_test)
