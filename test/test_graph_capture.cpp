// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Graph-capture feature tests (P1a). The seven CUDA-shape entry points
// implement record-once / replay-many capture on top of the epoch recording
// system: hazeStreamBeginCapture opens recording, hazeStreamEndCapture
// snapshots the recorded FHETCH op-sequence into a hazeGraph_t,
// hazeGraphInstantiate prepares a re-dispatchable hazeGraphExec_t, and each
// hazeGraphLaunch replays the snapshot deterministically. The [unit] cases
// cover argument validation and capture-state-machine transitions that return
// before any backend work; the [integration] cases drive real compute ops
// through the in-process FHETCH simulator and cover replay determinism,
// pre-launch non-materialization, independent graph/exec lifetime, same-
// topology exec update, topology-mismatch rejection, and destroyed-handle
// rejection.

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

// Assertion-safe RAII wrappers. Each is installed immediately after a
// successful acquisition; the destructor releases the resource even when an
// intervening REQUIRE fails and unwinds the stack. release() relinquishes
// ownership so a case that destroys a handle explicitly (to exercise destroy
// semantics) does not double-release it.

class DeviceGuard {
  public:
    explicit DeviceGuard(void *ptr) noexcept : ptr_(ptr) {}
    DeviceGuard(const DeviceGuard &) = delete;
    DeviceGuard &operator=(const DeviceGuard &) = delete;
    ~DeviceGuard() {
        if (ptr_ != nullptr)
            (void)hazeFree(ptr_);
    }
    void release() noexcept { ptr_ = nullptr; }

  private:
    void *ptr_;
};

class GraphGuard {
  public:
    explicit GraphGuard(hazeGraph_t graph) noexcept : graph_(graph) {}
    GraphGuard(const GraphGuard &) = delete;
    GraphGuard &operator=(const GraphGuard &) = delete;
    ~GraphGuard() {
        if (graph_ != nullptr)
            (void)hazeGraphDestroy(graph_);
    }
    void release() noexcept { graph_ = nullptr; }

  private:
    hazeGraph_t graph_;
};

class ExecGuard {
  public:
    explicit ExecGuard(hazeGraphExec_t exec) noexcept : exec_(exec) {}
    ExecGuard(const ExecGuard &) = delete;
    ExecGuard &operator=(const ExecGuard &) = delete;
    ~ExecGuard() {
        if (exec_ != nullptr)
            (void)hazeGraphExecDestroy(exec_);
    }
    void release() noexcept { exec_ = nullptr; }

  private:
    hazeGraphExec_t exec_;
};

// Drains an open capture on scope exit so a case that leaves capture active
// (whether by design or after a failed assertion) cannot leak capture state
// into the next case. Ending a capture that is not active is a harmless
// validation failure; a drained non-empty capture's graph is destroyed.
class CaptureGuard {
  public:
    CaptureGuard() noexcept = default;
    CaptureGuard(const CaptureGuard &) = delete;
    CaptureGuard &operator=(const CaptureGuard &) = delete;
    ~CaptureGuard() {
        hazeGraph_t graph = nullptr;
        (void)hazeStreamEndCapture(nullptr, &graph);
        if (graph != nullptr)
            (void)hazeGraphDestroy(graph);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// [unit] null-handle / argument-validation cases. These return before any
// backend work, so they need no compute configuration. Each records the
// translated error in the thread-local last-error register, which clears on
// read.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: hazeGraphDestroy rejects a null graph", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphDestroy(nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: hazeGraphExecDestroy rejects a null exec", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphExecDestroy(nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: hazeGraphInstantiate rejects a null exec out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphInstantiate(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: hazeGraphInstantiate zeroes the exec out-pointer on a null graph",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // A non-null exec out-pointer is zeroed on entry-validation failure so a
    // caller that ignores the error code cannot observe an uninitialised handle.
    hazeGraphExec_t exec = reinterpret_cast<hazeGraphExec_t>(0x1);
    REQUIRE(hazeGraphInstantiate(&exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(exec == nullptr);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: hazeGraphLaunch rejects a null exec", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: hazeGraphExecUpdate rejects null arguments", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGraphExecUpdate(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: hazeStreamEndCapture rejects a null graph out-pointer while capturing",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    CaptureGuard capture_drain;
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeStreamEndCapture(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// [unit] capture state-machine transitions.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: begin capture succeeds and accepts a null stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    CaptureGuard capture_drain;
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
}

TEST_CASE("graph capture: end capture with nothing recorded reports source unavailable", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    hazeGraph_t graph = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    REQUIRE(graph == nullptr);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_SOURCE_UNAVAILABLE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("graph capture: a nested begin is rejected and capture state recovers", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        // A second begin while a capture is already active is an illegal-state
        // transition and leaves the first capture untouched.
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    }
    // After the guard drains the capture, a fresh begin/end cycle works,
    // proving the state machine recovered.
    CaptureGuard capture_drain;
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
}

TEST_CASE("graph capture: end capture without an active capture is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Not capturing: ending is an illegal-state transition distinct from the
    // empty-capture (SOURCE_UNAVAILABLE) case, and it leaves the out-pointer null.
    hazeGraph_t graph = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(graph == nullptr);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    // The register cleared and a subsequent capture still opens.
    CaptureGuard capture_drain;
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
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
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    // Deterministic residue inputs (values < modulus) for a reproducible sum.
    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Record one compute op into the capture and tag its output.
    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // Non-materialization: the captured op has not run, so a device->host read
    // of the tagged output before the first launch reports the output as not
    // yet flushed rather than returning stale or zeroed data.
    std::vector<uint64_t> pre(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(pre.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();

    // First replay materializes the output.
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
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
    REQUIRE(out1 == expected);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
}

TEST_CASE("graph capture: an instantiated exec outlives the graph it came from", "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // The exec owns its own snapshot: destroying the source graph does not
    // invalidate it, and it still replays deterministically afterwards.
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();

    std::vector<uint64_t> expected(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);

    for (int launch = 0; launch < 3; ++launch) {
        REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
        std::vector<uint64_t> out(kRingDim, 0xDEADBEEFULL);
        REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
        REQUIRE(out == expected);
    }

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
}

TEST_CASE("graph capture: a graph remains reusable after an exec is destroyed", "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    std::vector<uint64_t> expected(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);

    // Instantiate, launch, and destroy a first exec.
    hazeGraphExec_t exec1 = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec1, graph) == HAZE_SUCCESS);
    REQUIRE(exec1 != nullptr);
    ExecGuard ge1(exec1);
    REQUIRE(hazeGraphLaunch(exec1, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeGraphExecDestroy(exec1) == HAZE_SUCCESS);
    ge1.release();

    // The graph is still valid and can back a second, independent exec.
    hazeGraphExec_t exec2 = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec2, graph) == HAZE_SUCCESS);
    REQUIRE(exec2 != nullptr);
    ExecGuard ge2(exec2);
    REQUIRE(hazeGraphLaunch(exec2, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out == expected);

    REQUIRE(hazeGraphExecDestroy(exec2) == HAZE_SUCCESS);
    ge2.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
}

// ---------------------------------------------------------------------------
// [integration] exec update. A same-topology graph refreshes an exec and
// changes its results; a graph with the same output cardinality but a
// different operation, or a different output cardinality, is rejected.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: a same-topology exec update rebinds and changes results",
          "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *c = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&c, kBytes) == HAZE_SUCCESS);
    DeviceGuard gc(c);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    // Distinct second addend so the rebind provably changes the output.
    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    const std::vector<uint64_t> cvec = haze::test::make_residue(modulus, 0x5555ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(c, cvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // g1: dst = a + b (one op, one tagged output).
    hazeGraph_t g1 = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g1) == HAZE_SUCCESS);
    }
    REQUIRE(g1 != nullptr);
    GraphGuard gg1(g1);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, g1) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> r1(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(r1.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // g2: dst = a + c. Same op count and output cardinality as g1, different
    // input binding, so it is a valid same-topology refresh.
    hazeGraph_t g2 = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, c, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g2) == HAZE_SUCCESS);
    }
    REQUIRE(g2 != nullptr);
    GraphGuard gg2(g2);

    REQUIRE(hazeGraphExecUpdate(exec, g2) == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> r2(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(r2.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    std::vector<uint64_t> expected1(kRingDim);
    std::vector<uint64_t> expected2(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k) {
        expected1[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
        expected2[k] = haze::test::add_mod(avec[k], cvec[k], modulus);
    }
    REQUIRE(r1 == expected1);
    REQUIRE(r2 == expected2);
    REQUIRE(r2 != r1);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(g1) == HAZE_SUCCESS);
    gg1.release();
    REQUIRE(hazeGraphDestroy(g2) == HAZE_SUCCESS);
    gg2.release();
}

TEST_CASE("graph capture: exec update rejects a different-operation topology", "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst_add = nullptr;
    void *dst_mul = nullptr;
    void *dst_two = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst_add, kBytes) == HAZE_SUCCESS);
    DeviceGuard gadd(dst_add);
    REQUIRE(hazeMalloc(&dst_mul, kBytes) == HAZE_SUCCESS);
    DeviceGuard gmul(dst_mul);
    REQUIRE(hazeMalloc(&dst_two, kBytes) == HAZE_SUCCESS);
    DeviceGuard gtwo(dst_two);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x3333ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x4444ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // g_add: a single add with one tagged output.
    hazeGraph_t g_add = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst_add, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst_add) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g_add) == HAZE_SUCCESS);
    }
    REQUIRE(g_add != nullptr);
    GraphGuard gg_add(g_add);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, g_add) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // g_mul: a single multiply with one tagged output. Same output cardinality
    // as g_add (one), but a genuinely different operation, so refreshing the
    // add-exec from it is a topology mismatch rather than a parameter rebind.
    hazeGraph_t g_mul = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMul(dst_mul, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst_mul) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g_mul) == HAZE_SUCCESS);
    }
    REQUIRE(g_mul != nullptr);
    GraphGuard gg_mul(g_mul);

    REQUIRE(hazeGraphExecUpdate(exec, g_mul) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    // g_two: two adds and two tagged outputs — a different output cardinality,
    // also a topology mismatch.
    hazeGraph_t g_two = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst_two, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMul(dst_add, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst_two) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst_add) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g_two) == HAZE_SUCCESS);
    }
    REQUIRE(g_two != nullptr);
    GraphGuard gg_two(g_two);

    REQUIRE(hazeGraphExecUpdate(exec, g_two) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    // The exec still refers to its original topology and launches successfully.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out.data(), dst_add, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    std::vector<uint64_t> expected(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
    REQUIRE(out == expected);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(g_add) == HAZE_SUCCESS);
    gg_add.release();
    REQUIRE(hazeGraphDestroy(g_mul) == HAZE_SUCCESS);
    gg_mul.release();
    REQUIRE(hazeGraphDestroy(g_two) == HAZE_SUCCESS);
    gg_two.release();
}

// ---------------------------------------------------------------------------
// [integration] destroyed-handle and per-argument validation using only real
// handles that were validly created and then destroyed (no fabricated
// dereferenceable pointers). The entry points validate each handle against the
// live registry and zero their output on failure.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: destroyed graph and exec handles are rejected", "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);

    // Destroy the exec, then the stale handle is rejected everywhere it is used.
    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphExecUpdate(exec, graph) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    // Destroy the graph, then the stale graph handle is rejected and any exec
    // out-pointer is zeroed on failure.
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
    hazeGraphExec_t exec2 = reinterpret_cast<hazeGraphExec_t>(0x1);
    REQUIRE(hazeGraphInstantiate(&exec2, graph) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(exec2 == nullptr);
    hazeGetLastError();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("graph capture: exec and graph entries validate each argument independently",
          "[integration]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // A null out-pointer is rejected even when the graph is valid.
    REQUIRE(hazeGraphInstantiate(nullptr, graph) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    // Update rejects a null exec with a valid graph, and a valid exec with a
    // null graph, independently.
    REQUIRE(hazeGraphExecUpdate(nullptr, graph) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphExecUpdate(exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    // State recovery: after the rejected calls the valid exec still launches.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    std::vector<uint64_t> expected(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
    REQUIRE(out == expected);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
}

// ---------------------------------------------------------------------------
// [integration] T1 discriminators. The cases above establish the happy path but
// still pass against two historical defects: caching output VALUES at capture
// and re-injecting them on launch (instead of really replaying), and comparing
// only output ADDRESSES in exec-update (instead of the recorded topology). The
// four cases below fail against those defects specifically.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: replay uses the inputs frozen at capture, not later device writes",
          "[integration]") {
    // Record-and-replay contract (the CUDA-graph "same arguments" analogue):
    // EndCapture snapshots the recorded project — including the operand residues
    // serialized at capture — and every launch re-dispatches THAT frozen project.
    // Overwriting the operand device memory at the same DevAddr after capture must
    // therefore NOT change a later replay. (A build that cached the first output
    // would also look "stable" here; the free/recycle case below is what a genuine
    // re-dispatch must pass and a cache cannot.)
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out1(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out1.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    // Overwrite operand a at the SAME DevAddr with a different residue.
    const std::vector<uint64_t> avec2 = haze::test::make_residue(modulus, 0x7777ULL, kRingDim);
    REQUIRE(avec2 != avec);
    REQUIRE(hazeMemcpy(a, avec2.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // The replay is unchanged: it reflects the residues captured at EndCapture,
    // not the later device write.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out2(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out2.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out2 == out1);

    std::vector<uint64_t> expected(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
    REQUIRE(out1 == expected);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
}

TEST_CASE("graph capture: launch after the output is freed and its address recycled is rejected",
          "[integration]") {
    // G6 ABA defense proves the launch really re-dispatches (a cache would blindly
    // re-inject): the snapshot recorded the output DevAddr's allocation generation
    // at capture, so once that buffer is freed and its address recycled into a new
    // allocation the generation no longer matches and the launch is rejected with
    // SOURCE_UNAVAILABLE instead of clobbering the unrelated new buffer.
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // Free the tagged output and reacquire a buffer. The allocator's LIFO free
    // list typically hands back the same DevAddr — the dangerous ABA case — but
    // the generation was bumped, so the recorded generation is now stale.
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    void *recycled = nullptr;
    REQUIRE(hazeMalloc(&recycled, kBytes) == HAZE_SUCCESS);
    DeviceGuard grecycled(recycled);

    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_SOURCE_UNAVAILABLE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    // The rejected launch did not write into the recycled buffer: it was never
    // tagged/flushed, so a device->host read still reports it as not flushed.
    std::vector<uint64_t> probe(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(probe.data(), recycled, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
}

TEST_CASE("graph capture: device reset invalidates captured graphs and execs", "[integration]") {
    // hazeDeviceReset tears down the runtime, which must also drop every captured
    // graph and instantiated exec (graph_reset). The monotonic handle-id counter
    // is deliberately NOT reset, so a stale token can never alias a post-reset one
    // and is rejected everywhere, and a fresh capture cycle still works.
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    }
    REQUIRE(graph != nullptr);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);

    // Reset drops the device allocations AND the graph/exec registries.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // Every stale handle is rejected; none is silently reused.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphExecUpdate(exec, graph) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    // The runtime still works after the reset: a fresh capture/instantiate/launch
    // cycle succeeds with freshly minted (non-aliasing) handles.
    const uint64_t modulus2 = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);
    REQUIRE(modulus2 == modulus);
    void *a2 = nullptr;
    void *b2 = nullptr;
    void *dst2 = nullptr;
    REQUIRE(hazeMalloc(&a2, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga2(a2);
    REQUIRE(hazeMalloc(&b2, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb2(b2);
    REQUIRE(hazeMalloc(&dst2, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst2(dst2);
    REQUIRE(hazeMemcpy(a2, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b2, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t graph2 = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst2, a2, b2, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst2) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graph2) == HAZE_SUCCESS);
    }
    REQUIRE(graph2 != nullptr);
    GraphGuard gg2(graph2);
    REQUIRE(graph2 != graph); // fresh token, does not alias the pre-reset graph

    hazeGraphExec_t exec2 = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec2, graph2) == HAZE_SUCCESS);
    REQUIRE(exec2 != nullptr);
    ExecGuard ge2(exec2);
    REQUIRE(exec2 != exec); // fresh token, does not alias the pre-reset exec
    REQUIRE(hazeGraphLaunch(exec2, nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeGraphExecDestroy(exec2) == HAZE_SUCCESS);
    ge2.release();
    REQUIRE(hazeGraphDestroy(graph2) == HAZE_SUCCESS);
    gg2.release();
}

TEST_CASE("graph capture: exec update rejects a different op even at the same output address",
          "[integration]") {
    // Isolates topology from output-address identity: g_add and g_mul write to the
    // SAME dst DevAddr and tag the SAME output, so the ONLY difference is the
    // opcode. An update that compared output addresses would wrongly accept it; the
    // topology fingerprint makes add-vs-mul a genuine mismatch and rejects it.
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    hazeGraph_t g_add = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g_add) == HAZE_SUCCESS);
    }
    REQUIRE(g_add != nullptr);
    GraphGuard gg_add(g_add);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, g_add) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // g_mul writes to the SAME dst address and tags the SAME output; only the
    // operation differs.
    hazeGraph_t g_mul = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMul(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g_mul) == HAZE_SUCCESS);
    }
    REQUIRE(g_mul != nullptr);
    GraphGuard gg_mul(g_mul);

    REQUIRE(hazeGraphExecUpdate(exec, g_mul) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    // The exec still holds its original add topology and replays add semantics.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    std::vector<uint64_t> expected(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected[k] = haze::test::add_mod(avec[k], bvec[k], modulus);
    REQUIRE(out == expected);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(g_add) == HAZE_SUCCESS);
    gg_add.release();
    REQUIRE(hazeGraphDestroy(g_mul) == HAZE_SUCCESS);
    gg_mul.release();
}
