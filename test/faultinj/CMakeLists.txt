# ============================================================================
# Track 04 (W6) — CXL non-coherence fault-injection layer.
#
# Lives under test/ (per sessions/00-BACKGROUND.md: src/cxl_transport/ belongs to
# Track 03; Track 04 uses test/faultinj/). This file defines the targets but does
# NOT wire itself into the build: per the same doc, root CMakeLists.txt option()
# and the add_subdirectory()/embarlet-link additions are the CMake integrator's
# (Track 01 / human-at-merge) job. See sessions/handoff-04.md for the exact hunks.
# Until then, build+run the harness standalone with ./smoke_build.sh (no CMake).
# ============================================================================

# Controller/adversary library. The CXL_FAULT_INJECTION compile definition is
# PUBLIC so it propagates to every target that links this library (the harness,
# and embarlet once Track 01 links it), activating the interim interposition hooks
# in the accessor seam (common/performance_utils.h). Targets that do NOT link this
# library keep the production accessor code (OFF => zero footprint).
add_library(cxl_faultinj STATIC
    cxl_fault_inject.cc
    cxl_fault_inject.h
)

# PUBLIC include dirs propagate to consumers (incl. embarlet) so that, under the
# flag, common/performance_utils.h can resolve "faultinj/cxl_fault_inject.h"
# (via test/) and this layer can resolve "common/..." (via src/).
target_include_directories(cxl_faultinj PUBLIC
    ${CMAKE_SOURCE_DIR}/test
    ${CMAKE_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}
)

target_compile_definitions(cxl_faultinj PUBLIC CXL_FAULT_INJECTION)

target_link_libraries(cxl_faultinj PUBLIC
    Threads::Threads
)

# --------------------------------------------------------------------------
# In-vitro harness — standalone, no cluster / no /dev/shm / no lock required.
# --------------------------------------------------------------------------
add_executable(faultinj_harness
    faultinj_harness.cc
)

target_include_directories(faultinj_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/test
    ${CMAKE_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}
)

target_link_libraries(faultinj_harness PRIVATE
    cxl_faultinj
    glog::glog
    Threads::Threads
)

set_target_properties(faultinj_harness PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Register as a CTest (pure-logic, needs no testbed lock). RUN_SERIAL because the
# torn-window "caught" verdict is a genuine race outcome — under `ctest -j` on a
# core-starved runner a co-scheduled load could deprive the reader thread and mask
# the catch. Serial + the in-harness retry keep it deterministic-green.
add_test(NAME faultinj_harness
    COMMAND faultinj_harness 50000
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_tests_properties(faultinj_harness PROPERTIES
    TIMEOUT 120
    RUN_SERIAL TRUE
    LABELS "unit;faultinj;cxl;noncoherence;correctness")
