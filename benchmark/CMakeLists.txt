# haze_benchmarks — Google Benchmark microbenchmark harness for the FHE compute
# ops (add/mul/NTT/automorph, SRP + MRP) and the record->flush->replay path
# (P1b). Built ONLY when -DHAZE_BUILD_BENCHMARKS=ON (see the parent
# CMakeLists.txt): it is never part of the default build, never part of the CI
# merge gates, and never absorbed into the shipped libhaze, so the library, its
# ABI surface, and the symbol-leak audit are byte-for-byte unaffected.
#
# The harness links the shipped libhaze.so (the haze* C ABI only) as an external
# consumer would; the two translation units include only the public <haze/*.h>
# headers, so no OpenFHE/fhetch include dirs or CPROBES wiring are needed here.
# benchmark::benchmark_main supplies the single main(), so neither TU defines one.

# Resolve Google Benchmark. The parent already requires it before entering this
# subdirectory; repeating the CONFIG lookup is idempotent and keeps this file
# self-contained.
find_package(benchmark CONFIG REQUIRED)

add_executable(haze_benchmarks
  bench_compute.cpp
  bench_record_replay.cpp
)

# C++23, matching the rest of the tree (inherited from the root, asserted here).
set_target_properties(haze_benchmarks PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON)

# <haze/haze.h> and <haze/haze_types.h> arrive transitively from the `haze`
# target's PUBLIC include dir. <haze/replay_bridge.h> lives in the bridge's own
# public include dir, so name that directory directly.
target_include_directories(haze_benchmarks PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../replay_bridge/include
)

# haze                      — the shipped shared object; exports the haze* C ABI
#                             and carries the public include dir transitively.
# benchmark::benchmark_main — Google Benchmark plus its default main().
# Threads::Threads          — explicit, matching the other haze executables.
target_link_libraries(haze_benchmarks PRIVATE
  haze
  benchmark::benchmark_main
  Threads::Threads
)

# Land the binary at the top of the build tree (build/haze_benchmarks) next to
# haze_tests / haze_e2e_tests rather than in build/benchmark/, so `make bench`
# and the benchmark workflow can reference it by the same $(BUILD_DIR)/<exe>
# convention.
set_target_properties(haze_benchmarks PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
