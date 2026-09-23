# Embarcadero Test Suite

# End-to-End Tests (Shell scripts)
# These run actual broker clusters and validate system behavior

option(EMBARCADERO_ENABLE_UNSAFE_LEGACY_E2E
    "Register legacy hardware scripts with unsafe shared-resource cleanup" OFF)
if(EMBARCADERO_ENABLE_UNSAFE_LEGACY_E2E)
    message(WARNING "Legacy E2E scripts can kill unrelated brokers and remove shared readiness files; run only on a dedicated test host")
    set(_e2e_environment
        "EMBARCADERO_TEST_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "EMBARCADERO_TEST_BROKER=$<TARGET_FILE:embarlet>"
        "EMBARCADERO_TEST_CLIENT=$<TARGET_FILE:throughput_test>")
# Add E2E tests as custom targets
add_custom_target(test_e2e
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -L legacy_e2e --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running end-to-end tests"
    VERBATIM
)

# Individual E2E tests
#
# `e2e/test_basic_publish.sh` is deliberately retired: it predates the
# sequencer/readiness contract and used broad process killing during cleanup.
# Publication-relevant end-to-end cells use scripts/run_multiclient.sh, which
# owns exact child PIDs and records a run contract/manifest.  Do not add the
# legacy script back to CTest.

add_test(
    NAME e2e_explicit_replication_order5_ack2
    COMMAND ${CMAKE_COMMAND} -E env ${_e2e_environment} bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_explicit_replication_order5_ack2.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(
    NAME e2e_order5_live_session_fence_fire
    COMMAND ${CMAKE_COMMAND} -E env ${_e2e_environment} bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_order5_live_session_fence_fire.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(
    NAME e2e_order5_multibroker_unacked_release
    COMMAND ${CMAKE_COMMAND} -E env ${_e2e_environment} bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_order5_multibroker_unacked_release.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Set test properties
set_tests_properties(e2e_explicit_replication_order5_ack2 PROPERTIES
    TIMEOUT 600  # 10 minutes max (replication may take longer)
    RUN_SERIAL TRUE
    LABELS "legacy_e2e;e2e;replication;order5;ack2"
)

set_tests_properties(e2e_order5_live_session_fence_fire PROPERTIES
    TIMEOUT 180
    RUN_SERIAL TRUE
    LABELS "legacy_e2e;e2e;order5;session;fencing;correctness"
)

set_tests_properties(e2e_order5_multibroker_unacked_release PROPERTIES
    TIMEOUT 180
    RUN_SERIAL TRUE
    LABELS "legacy_e2e;e2e;order5;session;multibroker;correctness"
)

endif()

# Unit tests
find_package(PkgConfig REQUIRED)
if(PkgConfig_FOUND)
    pkg_check_modules(GTEST REQUIRED gtest)
    find_path(GMOCK_INCLUDE_DIR gmock/gmock.h REQUIRED)
    list(APPEND GTEST_INCLUDE_DIRS "${GMOCK_INCLUDE_DIR}")
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
            glog::glog
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

        # Uses two real processes, a MAP_SHARED frontier (local CXL emulation),
        # SIGKILL between pwrite and fdatasync, then restart/redrive.  This is
        # intentionally separate from the pure model above.
        add_executable(media_durability_process_restart_test
            media_durability_process_restart_test.cc
        )
        target_include_directories(media_durability_process_restart_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(media_durability_process_restart_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(media_durability_process_restart_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME integration_media_durability_process_restart
                 COMMAND media_durability_process_restart_test)
        set_tests_properties(integration_media_durability_process_restart PROPERTIES
            TIMEOUT 60
            LABELS "integration;ack2;durability;process_restart;cxl_emulation"
        )

        add_executable(scalog_replication_rpc_durability_test
            scalog_replication_rpc_durability_test.cc
            ../src/disk_manager/scalog_replication_client.cc
            ../src/disk_manager/scalog_replication_manager.cc
            ../src/common/configuration.cc
        )
        target_include_directories(scalog_replication_rpc_durability_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(scalog_replication_rpc_durability_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(scalog_replication_rpc_durability_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            scalog_replication_grpc_proto
            scalog_sequencer_grpc_proto
            protobuf::libprotobuf
            glog::glog
            gflags
            ${FOLLY_LIBRARIES}
            grpc++
            grpc++_reflection
            yaml-cpp
            pthread
        )
        add_test(NAME integration_scalog_replication_rpc_durability
                 COMMAND scalog_replication_rpc_durability_test)
        set_tests_properties(integration_scalog_replication_rpc_durability PROPERTIES
            TIMEOUT 60
            LABELS "integration;scalog;replication;durability;fdatasync"
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

        add_executable(baseline_control_transport_test
            baseline_control_transport_test.cc
            ../src/common/baseline_control_transport.cc
        )
        target_include_directories(baseline_control_transport_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(baseline_control_transport_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(baseline_control_transport_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_baseline_control_transport COMMAND baseline_control_transport_test)
        set_tests_properties(unit_baseline_control_transport PROPERTIES
            TIMEOUT 60
            LABELS "unit;baseline;control_transport;configuration"
        )

        add_executable(baseline_cxl_layout_test
            baseline_cxl_layout_test.cc
            ../src/cxl_transport/cxl_mailbox.cc
        )
        target_include_directories(baseline_cxl_layout_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(baseline_cxl_layout_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(baseline_cxl_layout_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            glog::glog
            absl::flat_hash_map
            heartbeat_grpc_proto
            protobuf::libprotobuf
            pthread
        )
        add_test(NAME unit_baseline_cxl_layout COMMAND baseline_cxl_layout_test)
        set_tests_properties(unit_baseline_cxl_layout PROPERTIES
            TIMEOUT 60
            LABELS "unit;baseline;cxl;layout"
        )

        add_executable(nt_memcpy_test
            nt_memcpy_test.cc
        )
        target_include_directories(nt_memcpy_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(nt_memcpy_test PRIVATE ${GTEST_CFLAGS_OTHER} -mclflushopt)
        target_link_libraries(nt_memcpy_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            pthread
        )
        add_test(NAME unit_nt_memcpy COMMAND nt_memcpy_test)
        set_tests_properties(unit_nt_memcpy PROPERTIES
            TIMEOUT 60
            LABELS "unit;cxl;nt;perf;correctness"
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
            ../src/client/publisher_session.cc
            ../src/client/publisher_retention.cc
            ../src/client/publisher_ack.cc
            ../src/client/subscriber.cc
            ../src/client/result_writer.cc
            ../src/common/configuration.cc
            ${EMBARCADERO_LEGACY_COMPAT_SOURCE}
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
            lazylog_metadata_grpc_proto
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            glog::glog
            gflags
            mimalloc
            heartbeat_grpc_proto
            session_proto
            corfu_sequencer_grpc_proto
            corfu_token_proxy_grpc_proto
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

        add_executable(order5_subscriber_reorder_test
            order5_subscriber_reorder_test.cc
            ../src/client/common.cc
            ../src/client/buffer.cc
            ../src/client/queue_buffer.cc
            ../src/client/publisher.cc
            ../src/client/publisher_session.cc
            ../src/client/publisher_retention.cc
            ../src/client/publisher_ack.cc
            ../src/client/subscriber.cc
            ../src/client/result_writer.cc
            ../src/common/configuration.cc
            ${EMBARCADERO_LEGACY_COMPAT_SOURCE}
        )
        target_include_directories(order5_subscriber_reorder_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/client
            ${CMAKE_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(order5_subscriber_reorder_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(order5_subscriber_reorder_test PRIVATE
            lazylog_metadata_grpc_proto
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            glog::glog
            gflags
            mimalloc
            heartbeat_grpc_proto
            session_proto
            corfu_sequencer_grpc_proto
            corfu_token_proxy_grpc_proto
            corfu_replication_grpc_proto
            scalog_replication_grpc_proto
            grpc++_reflection
            grpc++
            protobuf::libprotobuf
            ${FOLLY_LIBRARIES}
            Threads::Threads
            yaml-cpp
        )
        add_test(NAME unit_order5_subscriber_reorder COMMAND order5_subscriber_reorder_test)
        set_tests_properties(unit_order5_subscriber_reorder PROPERTIES
            TIMEOUT 120
            LABELS "unit;order5;subscriber;reorder;correctness"
        )

        add_executable(baseline_transport_equivalence_test
            baseline_transport_equivalence_test.cc
            ../src/cxl_manager/scalog_global_ordering_core.cc
            ../src/cxl_manager/lazylog_binding_core.cc
        )
        target_include_directories(baseline_transport_equivalence_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${GTEST_INCLUDE_DIRS}
        )
        target_compile_options(baseline_transport_equivalence_test PRIVATE ${GTEST_CFLAGS_OTHER})
        target_link_libraries(baseline_transport_equivalence_test PRIVATE
            ${GTEST_LIBRARIES}
            ${GTEST_MAIN_LIB}
            glog::glog
            absl::flat_hash_map
            pthread
        )
        add_test(NAME unit_baseline_transport_equivalence COMMAND baseline_transport_equivalence_test)
        set_tests_properties(unit_baseline_transport_equivalence PROPERTIES
            TIMEOUT 60
            LABELS "unit;baseline;transport;equivalence;scalog;lazylog;correctness"
        )
    else()
        message(FATAL_ERROR "BUILD_TESTING requires GTest")
    endif()
else()
    message(FATAL_ERROR "BUILD_TESTING requires pkg-config")
endif()

# Test output directory
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_output)

# Add test dependencies
if(TARGET test_e2e)
    add_dependencies(test_e2e embarlet throughput_test)
endif()

# Small production configuration/layout checks; no shared-region allocation.
add_executable(configuration_safety_test
    configuration_safety_test.cc
    ../src/common/configuration.cc)
target_include_directories(configuration_safety_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR} ${GTEST_INCLUDE_DIRS})
target_link_libraries(configuration_safety_test PRIVATE
    ${GTEST_LIBRARIES} ${GTEST_MAIN_LIB} yaml-cpp glog::glog heartbeat_grpc_proto Threads::Threads)
add_test(NAME unit_configuration_safety COMMAND configuration_safety_test)
set_tests_properties(unit_configuration_safety PROPERTIES
    TIMEOUT 60 LABELS "unit;configuration;layout;correctness")

# Production reservation primitives exercised without a broker or large mapping.
add_executable(bounded_reservation_test bounded_reservation_test.cc)
target_include_directories(bounded_reservation_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bounded_reservation_test PRIVATE Threads::Threads atomic)
add_test(NAME unit_bounded_reservation COMMAND bounded_reservation_test)
set_tests_properties(unit_bounded_reservation PROPERTIES
    TIMEOUT 60 LABELS "unit;reservation;capacity;correctness")

add_executable(chain_shutdown_test
    chain_shutdown_test.cc ../src/disk_manager/chain_replication.cc)
target_include_directories(chain_shutdown_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR} ${GTEST_INCLUDE_DIRS})
target_link_libraries(chain_shutdown_test PRIVATE
    ${GTEST_LIBRARIES} ${GTEST_MAIN_LIB} glog::glog heartbeat_grpc_proto Threads::Threads)
add_test(NAME unit_chain_shutdown COMMAND chain_shutdown_test)
set_tests_properties(unit_chain_shutdown PROPERTIES
    TIMEOUT 10 LABELS "unit;replication;shutdown;correctness")

add_executable(network_safety_test network_safety_test.cc)
target_include_directories(network_safety_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR} ${GTEST_INCLUDE_DIRS})
target_link_libraries(network_safety_test PRIVATE
    ${GTEST_LIBRARIES} ${GTEST_MAIN_LIB} heartbeat_grpc_proto Threads::Threads)
add_test(NAME unit_network_safety COMMAND network_safety_test)
set_tests_properties(unit_network_safety PROPERTIES
    TIMEOUT 30 LABELS "unit;network;framing;shutdown;correctness")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME unit_dev_runner
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_dev_cluster.py)
set_tests_properties(unit_dev_runner PROPERTIES
    TIMEOUT 30 LABELS "unit;runner;cleanup;correctness")

add_test(NAME unit_perf_compare
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_perf_compare.py)
add_test(NAME unit_perf_analysis
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_analyze_perf_comparison.py)
set_tests_properties(unit_perf_compare unit_perf_analysis PROPERTIES
    TIMEOUT 30 LABELS "unit;performance;measurement;correctness")

foreach(admission IN ITEMS session_admission single_topic_admission)
    add_executable(${admission}_test ${admission}_test.cc)
    target_include_directories(${admission}_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${admission}_test PRIVATE Threads::Threads)
    add_test(NAME unit_${admission} COMMAND ${admission}_test)
    set_tests_properties(unit_${admission} PROPERTIES TIMEOUT 30 LABELS "unit;admission;correctness")
endforeach()

add_executable(test_shared_mapping shared_mapping_test.cc)
target_include_directories(test_shared_mapping PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(test_shared_mapping PRIVATE Threads::Threads)
add_test(NAME shared_mapping COMMAND test_shared_mapping)
set_tests_properties(shared_mapping PROPERTIES TIMEOUT 10 LABELS "unit;layout")

add_executable(fault_control_test fault_control_test.cc ${PROJECT_SOURCE_DIR}/src/common/fault_injection.cc)
target_include_directories(fault_control_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${GTEST_INCLUDE_DIRS})
target_compile_definitions(fault_control_test PRIVATE EMBARCADERO_ENABLE_FAULT_INJECTION=1)
target_link_libraries(fault_control_test PRIVATE ${GTEST_LIBRARIES} ${GTEST_MAIN_LIB} Threads::Threads)
add_test(NAME unit_fault_control COMMAND fault_control_test)
set_tests_properties(unit_fault_control PROPERTIES TIMEOUT 30 LABELS "unit;fault-control;correctness")

add_executable(production_fault_driver integration/production_fault_driver.cc
    integration/production_fault_support.cc integration/session_fault_scenarios.cc
    integration/replication_fault_scenarios.cc integration/session_capacity_scenario.cc
    integration/storage_fault_scenarios.cc)
target_include_directories(production_fault_driver PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR})
target_link_libraries(production_fault_driver PRIVATE embarcadero_configuration heartbeat_grpc_proto
    session_proto cxl_transport grpc++ protobuf::libprotobuf glog::glog Threads::Threads)
set_target_properties(production_fault_driver PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
add_test(NAME unit_production_fault_driver COMMAND production_fault_driver --self-test)
set_tests_properties(unit_production_fault_driver PROPERTIES TIMEOUT 30 LABELS "unit;protocol;correctness")

add_executable(test_support_contract support_contract_test.cc)
target_include_directories(test_support_contract PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME unit_support_contract COMMAND test_support_contract)
set_tests_properties(unit_support_contract PROPERTIES TIMEOUT 10 LABELS "unit;support-contract;correctness")

add_test(NAME unit_production_fault_harness
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/tools -p test_production_faults.py)
set_tests_properties(unit_production_fault_harness PROPERTIES TIMEOUT 30 LABELS "unit;integration-harness")

add_executable(test_memory_replica_sink memory_replica_sink_test.cc)
target_include_directories(test_memory_replica_sink PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME unit_memory_replica_sink COMMAND test_memory_replica_sink)
set_tests_properties(unit_memory_replica_sink PROPERTIES TIMEOUT 10 LABELS "unit;replication;correctness")

add_executable(test_epoch_shutdown epoch_shutdown_test.cc)
target_include_directories(test_epoch_shutdown PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR})
target_link_libraries(test_epoch_shutdown PRIVATE heartbeat_grpc_proto corfu_replication_grpc_proto
    scalog_replication_grpc_proto lazylog_metadata_grpc_proto glog::glog Threads::Threads)
add_test(NAME unit_epoch_shutdown COMMAND test_epoch_shutdown)
set_tests_properties(unit_epoch_shutdown PROPERTIES TIMEOUT 10 LABELS "unit;epoch;shutdown;correctness")

add_test(NAME unit_experiment_dispatch
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_experiment.py)
set_tests_properties(unit_experiment_dispatch PROPERTIES
    TIMEOUT 30 LABELS "unit;runner;dispatch;correctness")
add_test(NAME unit_experiment_workload
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_experiment_workload.py)
set_tests_properties(unit_experiment_workload PROPERTIES
    TIMEOUT 30 LABELS "unit;runner;workload;correctness")
add_test(NAME unit_client_cli_validation
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_client_cli_validation.py
        $<TARGET_FILE:throughput_test> ${PROJECT_SOURCE_DIR}/config/client.yaml)
set_tests_properties(unit_client_cli_validation PROPERTIES
    TIMEOUT 15 LABELS "unit;client;workload;correctness")
add_test(NAME unit_fault_packet_parser
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_fault_control.py)
set_tests_properties(unit_fault_packet_parser PROPERTIES TIMEOUT 10 LABELS "unit;fault-control;correctness")

# Bounded fixture linked to the real broker implementation, with no broker main.
# Keep dependency/source inventory identical to the broker as components evolve.
if(TARGET embarlet)
    find_package(cxxopts REQUIRED)
    get_target_property(_broker_fixture_sources embarlet SOURCES)
    list(REMOVE_ITEM _broker_fixture_sources embarlet/embarlet.cc)
    list(TRANSFORM _broker_fixture_sources PREPEND "${PROJECT_SOURCE_DIR}/src/")
    get_target_property(_broker_fixture_links embarlet LINK_LIBRARIES)
    add_executable(topic_publication_test topic_publication_test.cc cxl_manager_geometry_test.cc ${_broker_fixture_sources})
    target_include_directories(topic_publication_test PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR} ${PROJECT_BINARY_DIR}/src)
    target_link_libraries(topic_publication_test PRIVATE ${_broker_fixture_links})
    if(NOT EMBARCADERO_ENABLE_FAULT_INJECTION)
        target_sources(topic_publication_test PRIVATE ${PROJECT_SOURCE_DIR}/src/common/fault_injection.cc)
        target_compile_definitions(topic_publication_test PRIVATE EMBARCADERO_ENABLE_FAULT_INJECTION=1)
    endif()
    add_test(NAME unit_topic_publication COMMAND topic_publication_test)
    set_tests_properties(unit_topic_publication PROPERTIES TIMEOUT 30 LABELS "unit;session;publication;fault-control;correctness")
    add_test(NAME unit_cxl_manager_geometry COMMAND topic_publication_test --manager-geometry)
    set_tests_properties(unit_cxl_manager_geometry PROPERTIES TIMEOUT 30 LABELS "unit;layout;allocation;correctness")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(chain_disk_fault_test chain_disk_fault_test.cc
        ${PROJECT_SOURCE_DIR}/src/disk_manager/chain_replication.cc)
    target_include_directories(chain_disk_fault_test PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR} ${GTEST_INCLUDE_DIRS})
    target_link_libraries(chain_disk_fault_test PRIVATE
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIB} glog::glog heartbeat_grpc_proto Threads::Threads)
    target_link_options(chain_disk_fault_test PRIVATE
        -Wl,--wrap=pwrite -Wl,--wrap=fdatasync -Wl,--wrap=close)
    add_test(NAME unit_chain_disk_fault COMMAND chain_disk_fault_test)
    set_tests_properties(unit_chain_disk_fault PROPERTIES TIMEOUT 30 LABELS "unit;replication;disk;fault;correctness")
endif()
