cmake_minimum_required(VERSION 3.16)

# ── Embarcadero benchmarks ────────────────────────────────────
# End-to-end KV workload (publication-grade). Built as part of the main tree.
add_subdirectory(kv_store)

# Component micro-benchmarks (Google Benchmark). Optional: only built when the
# `benchmark` package is installed. Toggle with -DBUILD_BENCHMARKS=OFF.
option(BUILD_BENCHMARKS "Build component micro-benchmarks (benchmarks/micro)" ON)
if(BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(performance_test micro/performance_test.cc)
        target_link_libraries(performance_test
            PRIVATE
            benchmark::benchmark
            absl::hash
            absl::synchronization
            pthread
        )
        target_include_directories(performance_test PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
        set_target_properties(performance_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    else()
        message(WARNING "Google Benchmark not found, skipping benchmarks/micro/performance_test")
    endif()
endif()

# NOTE: benchmarks/sequencer (Makefile) and benchmarks/sequencer5_ablation
# (standalone CMake project) are built independently, not from this tree.
# See their respective README.md.
