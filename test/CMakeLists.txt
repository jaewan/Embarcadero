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

add_test(
    NAME e2e_order5_live_session_fence_fire
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_order5_live_session_fence_fire.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(
    NAME e2e_order5_multibroker_unacked_release
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_order5_multibroker_unacked_release.sh
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

set_tests_properties(e2e_order5_live_session_fence_fire PROPERTIES
    TIMEOUT 180
    LABELS "e2e;order5;session;fencing;correctness"
)

set_tests_properties(e2e_order5_multibroker_unacked_release PROPERTIES
    TIMEOUT 180
    LABELS "e2e;order5;session;multibroker;correctness"
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
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

        # Phase 2 Integration Test
        add_executable(phase2_integration_test
            phase2_integration_test.cc
        )
        target_include_directories(phase2_integration_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(phase2_integration_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(phase2_integration_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            absl::flat_hash_map
            heartbeat_grpc_proto
            protobuf::libprotobuf
            pthread
        )

        add_test(NAME unit_phase2_integration COMMAND phase2_integration_test)
        set_tests_properties(unit_phase2_integration PROPERTIES
            TIMEOUT 120
            LABELS "unit;phase2;integration;production"
        )

        # Phase 3 Recovery Integration Test
        add_executable(phase3_recovery_test
            phase3_recovery_test.cc
        )
        target_include_directories(phase3_recovery_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(phase3_recovery_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(phase3_recovery_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            absl::flat_hash_map
            heartbeat_grpc_proto
            protobuf::libprotobuf
            pthread
        )

        add_test(NAME unit_phase3_recovery COMMAND phase3_recovery_test)
        set_tests_properties(unit_phase3_recovery PROPERTIES
            TIMEOUT 180
            LABELS "unit;phase3;recovery;integration;production"
        )

        add_executable(replication_topology_test
            replication_topology_test.cc
        )
        target_include_directories(replication_topology_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(replication_topology_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(replication_topology_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_replication_topology COMMAND replication_topology_test)
        set_tests_properties(unit_replication_topology PROPERTIES
            TIMEOUT 60
            LABELS "unit;replication;topology;smoke"
        )

        add_executable(chain_pipeline_test
            chain_pipeline_test.cc
        )
        target_include_directories(chain_pipeline_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(chain_pipeline_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(chain_pipeline_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_chain_pipeline COMMAND chain_pipeline_test)
        set_tests_properties(unit_chain_pipeline PROPERTIES
            TIMEOUT 60
            LABELS "unit;replication;chain;pipeline;correctness"
        )

        add_executable(durable_frontier_test
            durable_frontier_test.cc
        )
        target_include_directories(durable_frontier_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(durable_frontier_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(durable_frontier_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_durable_frontier COMMAND durable_frontier_test)
        set_tests_properties(unit_durable_frontier PROPERTIES
            TIMEOUT 60
            LABELS "unit;durability;frontier;correctness"
        )

        # SCALOG ACK invariant test
        add_executable(scalog_ack_invariant_test
            scalog_ack_invariant_test.cc
        )
        target_include_directories(scalog_ack_invariant_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(scalog_ack_invariant_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(scalog_ack_invariant_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_scalog_ack_invariant COMMAND scalog_ack_invariant_test)
        set_tests_properties(unit_scalog_ack_invariant PROPERTIES
            TIMEOUT 60
            LABELS "unit;scalog;ack;invariant;correctness"
        )

        add_executable(lazylog_latency_invariant_test
            lazylog_latency_invariant_test.cc
        )
        target_include_directories(lazylog_latency_invariant_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(lazylog_latency_invariant_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(lazylog_latency_invariant_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_lazylog_latency_invariant COMMAND lazylog_latency_invariant_test)
        set_tests_properties(unit_lazylog_latency_invariant PROPERTIES
            TIMEOUT 60
            LABELS "unit;lazylog;latency;correctness"
        )

        add_executable(lazylog_metadata_replica_test
            lazylog_metadata_replica_test.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/cxl_manager/lazylog_metadata_replica.cc
        )
        target_include_directories(lazylog_metadata_replica_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(lazylog_metadata_replica_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(lazylog_metadata_replica_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            lazylog_metadata_grpc_proto
            grpc++
            protobuf::libprotobuf
            pthread
        )
        add_test(NAME unit_lazylog_metadata_replica COMMAND lazylog_metadata_replica_test)
        set_tests_properties(unit_lazylog_metadata_replica PROPERTIES
            TIMEOUT 60
            LABELS "unit;lazylog;metadata;durability;correctness"
        )

        add_executable(session_entry_torn_read_test
            session_entry_torn_read_test.cc
        )
        target_include_directories(session_entry_torn_read_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(session_entry_torn_read_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(session_entry_torn_read_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            heartbeat_grpc_proto
            protobuf::libprotobuf
            pthread
        )
        add_test(NAME unit_session_entry_torn_read COMMAND session_entry_torn_read_test)
        set_tests_properties(unit_session_entry_torn_read PROPERTIES
            TIMEOUT 60
            LABELS "unit;session_entry;torn_read;correctness"
        )

        add_executable(session_proto_roundtrip_test
            session_proto_roundtrip_test.cc
        )
        target_include_directories(session_proto_roundtrip_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(session_proto_roundtrip_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(session_proto_roundtrip_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            session_proto
            protobuf::libprotobuf
            pthread
        )
        add_test(NAME unit_session_proto_roundtrip COMMAND session_proto_roundtrip_test)
        set_tests_properties(unit_session_proto_roundtrip PROPERTIES
            TIMEOUT 60
            LABELS "unit;session;protobuf;correctness"
        )

        add_executable(durable_ack2_power_loss_model_test
            durable_ack2_power_loss_model_test.cc
        )
        target_include_directories(durable_ack2_power_loss_model_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(durable_ack2_power_loss_model_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(durable_ack2_power_loss_model_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_durable_ack2_power_loss_model COMMAND durable_ack2_power_loss_model_test)
        set_tests_properties(unit_durable_ack2_power_loss_model PROPERTIES
            TIMEOUT 60
            LABELS "unit;ack2;durability;power_loss;correctness"
        )

        add_executable(order5_recovery_property_test
            order5_recovery_property_test.cc
        )
        target_include_directories(order5_recovery_property_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(order5_recovery_property_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(order5_recovery_property_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_order5_recovery_property COMMAND order5_recovery_property_test)
        set_tests_properties(unit_order5_recovery_property PROPERTIES
            TIMEOUT 60
            LABELS "unit;order5;recovery;property;correctness"
        )

        add_executable(ack_rf_policy_test
            ack_rf_policy_test.cc
        )
        target_include_directories(ack_rf_policy_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(ack_rf_policy_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(ack_rf_policy_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_ack_rf_policy COMMAND ack_rf_policy_test)
        set_tests_properties(unit_ack_rf_policy PROPERTIES
            TIMEOUT 60
            LABELS "unit;ack;rf;contract;correctness"
        )

        add_executable(order5_session_fencing_test
            order5_session_fencing_test.cc
        )
        target_include_directories(order5_session_fencing_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(order5_session_fencing_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(order5_session_fencing_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_order5_session_fencing COMMAND order5_session_fencing_test)
        set_tests_properties(unit_order5_session_fencing PROPERTIES
            TIMEOUT 60
            LABELS "unit;order5;session;fencing;correctness"
        )

        add_executable(order5_publisher_rollover_test
            order5_publisher_rollover_test.cc
            ../src/client/common.cc
            ../src/client/buffer.cc
            ../src/client/queue_buffer.cc
            ../src/client/publisher.cc
            ../src/client/subscriber.cc
            ../src/client/result_writer.cc
            ../src/common/configuration.cc
            ../src/common/compat_isoc23.cpp
        )
        target_include_directories(order5_publisher_rollover_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/client
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(order5_publisher_rollover_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(order5_publisher_rollover_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            glog::glog
            gflags
            mimalloc
            heartbeat_grpc_proto
            session_proto
            corfu_sequencer_grpc_proto
            corfu_replication_grpc_proto
            scalog_replication_grpc_proto
            grpc++_reflection
            grpc++
            protobuf::libprotobuf
            ${FOLLY_LIBRARIES}
            Threads::Threads
            yaml-cpp
        )
        add_test(NAME unit_order5_publisher_rollover COMMAND order5_publisher_rollover_test)
        set_tests_properties(unit_order5_publisher_rollover PROPERTIES
            TIMEOUT 120
            LABELS "unit;order5;publisher;rollover;correctness"
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
