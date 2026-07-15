# haze_benchmarks — Google Benchmark microbenchmark harness for the FHE compute
# ops (add/mul/NTT/automorph, SRP + MRP) and the record->flush->replay path
# (P1b). Built ONLY when -DHAZE_BUILD_BENCHMARKS=ON (see the parent
# CMakeLists.txt): it is never part of the default build, never part of the CI
# merge gates, and never absorbed into the shipped libhaze, so the ABI surface
# and the symbol-leak audit are unaffected.
#
# The harness links the SHIPPED libhaze.so (the haze* C ABI only) exactly as an
# external consumer such as FIDESlib would — it does NOT absorb haze_objs — so
# it measures the real artifact and stays clear of the internal haze:: symbols
# the version script hides. This is why the two benchmark translation units
# include only the public <haze/*.h> headers and no niobium/OpenFHE headers, and
# why no OPENFHE_CPROBES / fhetch usage requirements are needed here.
#
# benchmark::benchmark_main supplies the entry point (a single main()), so
# neither translation unit defines its own.

add_executable(haze_benchmarks
  bench_compute.cpp
  bench_record_replay.cpp
)

# <haze/haze.h> and <haze/haze_types.h> are carried transitively by the `haze`
# target's PUBLIC include directory. <haze/replay_bridge.h> lives in the bridge's
# own public include dir (the bridge is absorbed into libhaze as objects rather
# than exposed as a target here), so name that directory directly.
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
# and the workflow can reference it by the same $(BUILD_DIR)/<exe> convention.
set_target_properties(haze_benchmarks PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
