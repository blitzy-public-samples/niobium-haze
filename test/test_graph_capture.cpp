// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Graph-capture feature tests (P1a). The seven CUDA-shape entry points
// implement record-once / replay-many capture on top of the epoch recording
// system: hazeStreamBeginCapture opens recording, hazeStreamEndCapture
// snapshots the recorded FHETCH op-sequence into a hazeGraph_t,
// hazeGraphInstantiate prepares a re-dispatchable hazeGraphExec_t, and each
// hazeGraphLaunch replays the snapshot deterministically. The [unit] cases
// cover argument validation that returns before any backend work; the
// [integration] cases drive a real compute op through the in-process FHETCH
// simulator.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <vector>

namespace {

// NTT-friendly prime (q == 1 mod 2N for N=4096); shared with the compute suite.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

} // namespace

// ---------------------------------------------------------------------------
// [unit] null-handle / argument-validation cases. These return before any
// backend work, so they need no compute configuration.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: hazeGraphDestroy rejects a null graph", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphDestroy(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("graph capture: hazeGraphExecDestroy rejects a null exec", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphExecDestroy(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("graph capture: hazeGraphInstantiate rejects a null exec out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphInstantiate(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("graph capture: hazeGraphInstantiate rejects a null graph", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // A non-null exec out-pointer is zeroed on entry-validation failure.
    hazeGraphExec_t exec = reinterpret_cast<hazeGraphExec_t>(0x1);
    REQUIRE(hazeGraphInstantiate(&exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(exec == nullptr);
    hazeGetLastError();
}

TEST_CASE("graph capture: hazeGraphLaunch rejects a null exec", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("graph capture: hazeGraphExecUpdate rejects null arguments", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphExecUpdate(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("graph capture: hazeStreamEndCapture rejects a null graph out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeStreamEndCapture(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// [unit] begin-capture success and empty-capture reporting.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: begin capture succeeds and accepts a null stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
}

TEST_CASE("graph capture: end capture with nothing recorded reports source unavailable", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    hazeGraph_t graph = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    REQUIRE(graph == nullptr);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// [integration] full lifecycle + replay determinism through the in-process
// FHETCH simulator. Malloc / H2D are pure allocator ops and are not recorded;
// only the compute op is captured, and the same DevAddr operands stay stable
// across replays.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: a captured graph replays deterministically many times", "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    // Deterministic residue inputs (values < modulus) for a reproducible sum.
    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Record one compute op into the capture and tag its output.
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    hazeGraph_t graph = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    REQUIRE(graph != nullptr);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);

    // First replay.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out1(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out1.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // Second replay must be byte-identical to the first.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out2(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out2.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out1 == out2);

    // Both replays match the host oracle (element-wise modular add).
    std::vector<uint64_t> expected(kRingDim);
    for (uint64_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
    REQUIRE(out1 == expected);

    // A same-topology exec refresh succeeds.
    REQUIRE(hazeGraphExecUpdate(exec, graph) == HAZE_SUCCESS);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

TEST_CASE("graph capture: exec update rejects a different-topology graph", "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst1 = nullptr;
    void *dst2a = nullptr;
    void *dst2b = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst2a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst2b, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x3333ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x4444ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // g1 records one compute op and tags a single output.
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst1, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst1) == HAZE_SUCCESS);
    hazeGraph_t g1 = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &g1) == HAZE_SUCCESS);
    REQUIRE(g1 != nullptr);

    // g2 records two compute ops and tags two outputs.
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst2a, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMul(dst2b, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst2a) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst2b) == HAZE_SUCCESS);
    hazeGraph_t g2 = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &g2) == HAZE_SUCCESS);
    REQUIRE(g2 != nullptr);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, g1) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);

    // Refreshing a one-output exec from a two-output graph is a topology mismatch.
    REQUIRE(hazeGraphExecUpdate(exec, g2) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    REQUIRE(hazeGraphDestroy(g1) == HAZE_SUCCESS);
    REQUIRE(hazeGraphDestroy(g2) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst2a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst2b) == HAZE_SUCCESS);
}
