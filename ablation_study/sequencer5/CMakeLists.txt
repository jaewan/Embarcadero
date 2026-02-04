# Standalone sequencer5 ablation benchmark
# Tests sequence logic from docs/EMBARCADERO_DEFINITIVE_DESIGN.md
# No dependency on main Embarcadero src/ (gRPC, folly, etc.)

cmake_minimum_required(VERSION 3.14)
project(sequencer5_ablation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build with ASan: cmake -DASAN=ON ..  (for debugging segfaults / use-after-free)
option(ASAN "Build with AddressSanitizer" OFF)
if(ASAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

# Build with TSan: cmake -DTSAN=ON ..  (for OSDI/SOSP: confirm no data races)
option(TSAN "Build with ThreadSanitizer" OFF)
if(TSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O1 -fsanitize=thread -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()

add_executable(sequencer5_benchmark sequencer5_benchmark.cc)
if(ASAN OR TSAN)
  target_compile_options(sequencer5_benchmark PRIVATE -Wall -O1 -g)
else()
  target_compile_options(sequencer5_benchmark PRIVATE -Wall -O3)
endif()
target_link_libraries(sequencer5_benchmark PRIVATE pthread)

# Optional: link libnuma on Linux for NUMA-aware allocation fallback
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(NUMA_LIB numa)
  if(NUMA_LIB)
    target_link_libraries(sequencer5_benchmark PRIVATE ${NUMA_LIB})
  endif()
endif()
