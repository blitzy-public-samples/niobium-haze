cmake_minimum_required(VERSION 3.22)
project(haze_examples LANGUAGES C CXX)

# Standalone consumer build for the two runnable Haze examples migrated from the
# top-level README (quickstart.c, ckks22.cpp). It builds against an
# already-built libhaze and is intentionally NOT part of the top-level haze
# build (the root CMakeLists.txt does not add_subdirectory(examples)). Override
# the *_DIR cache variables below to point at an installed/relocated libhaze and
# stock OpenFHE. The compile/link recipe mirrors scripts/test_readme_examples.sh.

# --- Overridable locations (default to this repository's source/build tree) ---
set(HAZE_INCLUDE_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    CACHE PATH "Directory containing <haze/haze.h> and <haze/haze_types.h>")
set(HAZE_BRIDGE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../replay_bridge/include"
    CACHE PATH "Directory containing <haze/replay_bridge.h>")
set(HAZE_LIB_DIR            "${CMAKE_CURRENT_SOURCE_DIR}/../build"
    CACHE PATH "Directory containing the built libhaze")
set(STOCK_OPENFHE_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lib/openfhe-stock"
    CACHE PATH "Stock OpenFHE install prefix (needed only for the C++ ckks22 example)")

# Locate the already-built libhaze under HAZE_LIB_DIR only; do not fall back to
# system paths so the example links the freshly built library.
find_library(HAZE_LIBRARY NAMES haze PATHS "${HAZE_LIB_DIR}" NO_DEFAULT_PATH)
if(NOT HAZE_LIBRARY)
  message(FATAL_ERROR
    "libhaze not found under HAZE_LIB_DIR=${HAZE_LIB_DIR}. "
    "Build it first (e.g. `make build`) or pass -DHAZE_LIB_DIR=<dir-with-libhaze>.")
endif()

# ---- C example: quickstart (links only libhaze; no OpenFHE) ----
add_executable(quickstart quickstart.c)
set_target_properties(quickstart PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(quickstart PRIVATE -O2)
target_include_directories(quickstart PRIVATE "${HAZE_INCLUDE_DIR}" "${HAZE_BRIDGE_INCLUDE_DIR}")
target_link_libraries(quickstart PRIVATE "${HAZE_LIBRARY}")
# BUILD_RPATH reproduces the script's -Wl,-rpath so the binary finds libhaze at
# run time from the build tree without installation.
set_target_properties(quickstart PROPERTIES BUILD_RPATH "${HAZE_LIB_DIR}")

# ---- C++ example: ckks22 (links libhaze + stock OpenFHE) ----
# Guarded so the C example still builds when stock OpenFHE is not installed.
if(EXISTS "${STOCK_OPENFHE_DIR}/include/openfhe")
  find_package(Threads REQUIRED)
  add_executable(ckks22 ckks22.cpp)
  set_target_properties(ckks22 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_compile_options(ckks22 PRIVATE -O2)
  # Public haze headers first, then the stock OpenFHE headers as system includes
  # (OpenFHE headers do not survive the repo's strict warning set).
  target_include_directories(ckks22 PRIVATE "${HAZE_INCLUDE_DIR}" "${HAZE_BRIDGE_INCLUDE_DIR}")
  target_include_directories(ckks22 SYSTEM PRIVATE
    "${STOCK_OPENFHE_DIR}/include/openfhe"
    "${STOCK_OPENFHE_DIR}/include/openfhe/core"
    "${STOCK_OPENFHE_DIR}/include/openfhe/pke"
    "${STOCK_OPENFHE_DIR}/include/openfhe/binfhe")
  # Link OpenFHE by bare names via a link directory, mirroring the script's
  # -L<stock>/lib -lOPENFHE*; portable across .so/.dylib.
  target_link_directories(ckks22 PRIVATE "${STOCK_OPENFHE_DIR}/lib")
  # Wrap the OpenFHE trio in --no-as-needed/--as-needed so all three are recorded
  # as direct DT_NEEDED entries. Without this, the linker's default --as-needed
  # drops libOPENFHEbinfhe (ckks22.cpp uses no binfhe symbol directly), leaving it
  # only as a transitive dependency of libOPENFHEpke; the executable's DT_RUNPATH
  # is consulted by the loader only for direct dependencies, so binfhe would fail
  # to resolve at run time from the build tree.
  target_link_libraries(ckks22 PRIVATE
    "${HAZE_LIBRARY}"
    -Wl,--no-as-needed OPENFHEpke OPENFHEbinfhe OPENFHEcore -Wl,--as-needed
    Threads::Threads)
  set_target_properties(ckks22 PROPERTIES
    BUILD_RPATH "${HAZE_LIB_DIR};${STOCK_OPENFHE_DIR}/lib")
else()
  message(STATUS
    "examples: stock OpenFHE not found at ${STOCK_OPENFHE_DIR}; "
    "skipping the C++ ckks22 example (the C quickstart example still builds).")
endif()
