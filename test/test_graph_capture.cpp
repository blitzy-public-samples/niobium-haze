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
#include <filesystem>
#include <fstream>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <ios>
#include <set>
#include <string>
#include <system_error>
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

// Enumerates the private snapshot directories that the graph-capture machinery
// clones under the system temp path (secure_clone_project_dir creates each via
// mkdtemp with a "haze_graph_" prefix -- one per captured graph and one per
// instantiated exec). Diffing the set across an instantiate call isolates the
// directory the new exec owns, so a corruption test can target exactly that
// exec's on-disk replay artifacts without disturbing any other graph or exec.
// Best-effort and non-throwing: filesystem errors yield an empty/partial set
// rather than an exception, which simply fails the "exactly one new directory"
// REQUIRE below.
std::set<std::filesystem::path> list_exec_dirs() {
    std::set<std::filesystem::path> dirs;
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    if (ec)
        return dirs;
    for (std::filesystem::directory_iterator it(tmp, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path &entry = it->path();
        const std::string name = entry.filename().string();
        if (name.starts_with("haze_graph_"))
            dirs.insert(entry);
    }
    return dirs;
}

// Overwrites a file with a short run of non-serialized garbage, truncating any
// prior contents. A cereal binary archive reading this stream fails with
// "Failed to read N bytes from input stream" — precisely the partial/corrupt
// probe read Issue 3 must contain behind the C ABI rather than let escape as an
// uncaught C++ exception. Returns false if the stream could not be written.
bool clobber_file(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    static constexpr char kGarbage[] = "\x01\x02\x03haze-corrupt-not-a-valid-archive\xff\xfe";
    out.write(kGarbage, static_cast<std::streamsize>(sizeof(kGarbage) - 1));
    return out.good();
}

// Mirrors haze::test::setup_integration_compute_config but pins the program
// directory (via hazeSetProgramDirectory, forwarded at init) to a caller-chosen
// path so a test can reach and mutate the on-disk recording artifacts the
// launch reads. The pin must land after hazeDeviceReset and before the first
// compute, so it is issued here rather than after the helper returns.
uint64_t setup_compute_config_in_dir(const std::filesystem::path &program_dir) {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramDirectory(program_dir.string().c_str()) == HAZE_SUCCESS);
    haze::test::apply_target_from_env();
    REQUIRE(hazeSetReducedNoise(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t scaffold = 0; // built then overwritten from the trace; not used for results
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    return kQ0;
}

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

TEST_CASE("graph capture: relaunch recomputes from an input overwritten at a stable address",
          "[integration]") {
    // Record-once / replay-many-with-CURRENT-inputs contract: a captured graph
    // records the op-sequence, not the computed output. Overwriting an operand
    // at the same device address and relaunching the SAME exec (no exec update)
    // must recompute against the new bytes — not replay a value cached at
    // capture time. This is the CRITICAL defect: the prior implementation cached
    // the output at EndCapture and re-applied it every launch, so an overwrite
    // was silently ignored.
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
    const std::vector<uint64_t> bvec1 = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec1.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Capture dst = a + b and instantiate once.
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

    // Launch #1 computes a + bvec1.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out1(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out1.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    std::vector<uint64_t> expected1(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected1[k] = haze::test::add_mod(avec[k], bvec1[k], modulus);
    REQUIRE(out1 == expected1);

    // Overwrite b in place with different bytes, then relaunch the SAME exec.
    const std::vector<uint64_t> bvec2 = haze::test::make_residue(modulus, 0x9999ULL, kRingDim);
    REQUIRE(bvec2 != bvec1);
    REQUIRE(hazeMemcpy(b, bvec2.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out2(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out2.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    std::vector<uint64_t> expected2(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected2[k] = haze::test::add_mod(avec[k], bvec2[k], modulus);
    // The relaunch reflects the overwritten operand, and provably differs from
    // the first launch (the two addends were chosen distinct).
    REQUIRE(out2 == expected2);
    REQUIRE(out2 != out1);

    // A third overwrite and relaunch confirms replay-many keeps reading current
    // inputs rather than latching onto the first launch's result.
    const std::vector<uint64_t> bvec3 = haze::test::make_residue(modulus, 0x7777ULL, kRingDim);
    REQUIRE(hazeMemcpy(b, bvec3.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> out3(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out3.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    std::vector<uint64_t> expected3(kRingDim);
    for (std::size_t k = 0; k < kRingDim; ++k)
        expected3[k] = haze::test::add_mod(avec[k], bvec3[k], modulus);
    REQUIRE(out3 == expected3);

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

TEST_CASE("graph capture: recapture after a stable-address overwrite rebinds both operands",
          "[integration]") {
    // Regression for the graph-capture / exec-update corruption: recapturing
    // the SAME topology at the SAME device addresses AFTER a launch that
    // re-tagged one operand at a stable address must rebind BOTH operands, not
    // silently alias the overwritten operand onto the other. A launch resets
    // the libnbfhetch synthetic-address space, invalidating every input
    // polynomial the epoch still cached; a recapture that reused those cached
    // polynomials (skipping a re-tag) collapsed the two distinct operands onto
    // one synthetic address, so the replayed graph computed add(a, a) instead
    // of add(a, b2) and the same-topology exec update was spuriously rejected.
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kQ0, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst_add = nullptr;
    void *dst_mul = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst_add, kBytes) == HAZE_SUCCESS);
    DeviceGuard gda(dst_add);
    REQUIRE(hazeMalloc(&dst_mul, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdm(dst_mul);

    // Distinct operands so add(a, a)/mul(a, a) (the corruption) provably differ
    // from add(a, b2)/mul(a, b2) (the correct result) in every coefficient.
    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x1111ULL, kRingDim);
    const std::vector<uint64_t> b1vec = haze::test::make_residue(modulus, 0x2222ULL, kRingDim);
    const std::vector<uint64_t> b2vec = haze::test::make_residue(modulus, 0x3333ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, b1vec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // hazeMul is the pointwise product (a * b) mod q for evaluation-domain
    // residues (see test_compute.cpp), so the host reference is exact.
    const auto mul_mod = [](uint64_t x, uint64_t y, uint64_t q) {
        return static_cast<uint64_t>((static_cast<__uint128_t>(x) * static_cast<__uint128_t>(y)) %
                                     q);
    };

    // Two-op topology: dst_add = a + b, dst_mul = a * b. Both operands are the
    // SAME two live-in addresses, so a recapture must re-read and re-tag both.
    const auto capture_two_ops = [&](hazeGraph_t *out) {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst_add, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeMul(dst_mul, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst_add) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst_mul) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, out) == HAZE_SUCCESS);
    };

    hazeGraph_t g1 = nullptr;
    capture_two_ops(&g1);
    REQUIRE(g1 != nullptr);
    GraphGuard gg1(g1);

    hazeGraphExec_t ex1 = nullptr;
    REQUIRE(hazeGraphInstantiate(&ex1, g1) == HAZE_SUCCESS);
    REQUIRE(ex1 != nullptr);
    ExecGuard ge1(ex1);

    // First launch reads the current shadows (a, b1).
    REQUIRE(hazeGraphLaunch(ex1, nullptr) == HAZE_SUCCESS);

    // Overwrite b in place with different bytes, then relaunch the SAME exec.
    // Per D-34 the relaunch recomputes from the current shadow at the stable
    // address, so this must already reflect b2.
    REQUIRE(hazeMemcpy(b, b2vec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(ex1, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> expect_add(kRingDim);
    std::vector<uint64_t> expect_mul(kRingDim);
    std::vector<uint64_t> alias_add(kRingDim); // the corruption: add(a, a)
    std::vector<uint64_t> alias_mul(kRingDim); // the corruption: mul(a, a)
    for (std::size_t k = 0; k < kRingDim; ++k) {
        expect_add[k] = haze::test::add_mod(avec[k], b2vec[k], modulus);
        expect_mul[k] = mul_mod(avec[k], b2vec[k], modulus);
        alias_add[k] = haze::test::add_mod(avec[k], avec[k], modulus);
        alias_mul[k] = mul_mod(avec[k], avec[k], modulus);
    }

    std::vector<uint64_t> relaunch_add(kRingDim, 0xDEADBEEFULL);
    std::vector<uint64_t> relaunch_mul(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(relaunch_add.data(), dst_add, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(relaunch_mul.data(), dst_mul, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(relaunch_add == expect_add);
    REQUIRE(relaunch_mul == expect_mul);

    // Recapture the SAME topology at the SAME addresses AFTER the launch: the
    // corruption trigger. Both a (stale from the first capture) and b
    // (re-tagged by the overwrite after the launch's synthetic-address reset)
    // must rebind as two DISTINCT live-ins.
    hazeGraph_t g2 = nullptr;
    capture_two_ops(&g2);
    REQUIRE(g2 != nullptr);
    GraphGuard gg2(g2);

    // The same-topology update must be ACCEPTED. It was spuriously rejected
    // with HAZE_ERROR_INVALID_VALUE when the recaptured trace signature was
    // corrupted by the operand aliasing.
    REQUIRE(hazeGraphExecUpdate(ex1, g2) == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(ex1, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> upd_add(kRingDim, 0xDEADBEEFULL);
    std::vector<uint64_t> upd_mul(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(upd_add.data(), dst_add, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(upd_mul.data(), dst_mul, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(upd_add == expect_add);
    REQUIRE(upd_mul == expect_mul);
    REQUIRE(upd_add != alias_add);
    REQUIRE(upd_mul != alias_mul);

    // A fresh instantiate of the recaptured graph must likewise compute
    // add(a, b2) / mul(a, b2), never the aliased add(a, a) / mul(a, a).
    hazeGraphExec_t ex2 = nullptr;
    REQUIRE(hazeGraphInstantiate(&ex2, g2) == HAZE_SUCCESS);
    REQUIRE(ex2 != nullptr);
    ExecGuard ge2(ex2);
    REQUIRE(hazeGraphLaunch(ex2, nullptr) == HAZE_SUCCESS);
    std::vector<uint64_t> fresh_add(kRingDim, 0xDEADBEEFULL);
    std::vector<uint64_t> fresh_mul(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(fresh_add.data(), dst_add, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(fresh_mul.data(), dst_mul, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
    REQUIRE(fresh_add == expect_add);
    REQUIRE(fresh_mul == expect_mul);
    REQUIRE(fresh_add != alias_add);
    REQUIRE(fresh_mul != alias_mul);

    REQUIRE(hazeGraphExecDestroy(ex2) == HAZE_SUCCESS);
    ge2.release();
    REQUIRE(hazeGraphExecDestroy(ex1) == HAZE_SUCCESS);
    ge1.release();
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

TEST_CASE("graph capture: exec update rejects a same-address different-operation refresh",
          "[integration]") {
    // The output-address check alone cannot distinguish a parameter rebind from
    // an operation change when both graphs tag the SAME output address: an add
    // and a multiply that each write `dst` share an identical output-address
    // list. A refresh across such an operation change must still be rejected, so
    // the exec never silently starts computing a different function. The trace
    // signature (op-sequence hash) provides that discrimination.
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

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x3333ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x4444ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // g_add: dst = a + b.
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

    // g_mul: dst = a * b — the SAME tagged output address as g_add, but a
    // different operation. The output-address lists are identical, so only the
    // op-sequence signature can reject this refresh.
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

    // The rejected update left the exec on its original add topology.
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
// [integration] ABI exception-safety on a corrupt on-disk replay artifact
// (Issue 3). The record-once / replay-many launch path reads the recorded trace
// and deserializes each output probe from the exec's snapshot directory on every
// launch. A partial or corrupt on-disk artifact (e.g. two processes sharing a
// recording directory racing a probe write) must surface as a representable
// HAZE_ERROR_INTERNAL at the C ABI boundary, never as an uncaught cereal
// exception that reaches std::terminate / SIGABRT across the noexcept boundary.
// Each case corrupts exactly the private snapshot directory the new exec owns
// (isolated via list_exec_dirs()), so no other exec or the shared program
// directory is disturbed. The launch securely rebuilds the program directory
// from the exec's snapshot (remove_all + a fresh recursive copy), so the
// corrupted trace file the snapshot carries is what the backend actually reads.
// Surviving to the post-launch REQUIRE is itself the proof that no exception
// crossed the C ABI: a std::terminate would abort the process before the
// assertion could run.
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: a corrupt on-disk trace fails launch cleanly without crossing the C ABI",
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

    // Capture dst = a + b so the snapshot holds a real op-sequence trace.
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

    // Isolate the private snapshot directory the new exec owns by diffing the
    // set of exec directories across the instantiate call.
    const std::set<std::filesystem::path> before = list_exec_dirs();
    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);
    const std::set<std::filesystem::path> after = list_exec_dirs();

    std::filesystem::path exec_dir;
    std::size_t new_count = 0;
    for (const std::filesystem::path &d : after) {
        if (!before.contains(d)) {
            exec_dir = d;
            ++new_count;
        }
    }
    REQUIRE(new_count == 1);
    REQUIRE(!exec_dir.empty());

    // Corrupt every recorded trace file in the exec's snapshot directory.
    std::error_code ec;
    std::size_t clobbered = 0;
    for (std::filesystem::directory_iterator it(exec_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension() == ".fhetch") {
            REQUIRE(clobber_file(it->path()));
            ++clobbered;
        }
    }
    REQUIRE(clobbered >= 1);

    // The launch reads and re-dispatches the (now garbage) trace; the backend
    // parse failure is contained and translated to a representable error rather
    // than a silent success on unreadable input or an exception across the ABI.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    // The exec handle stays valid and tears down cleanly after the failed launch.
    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();
}

TEST_CASE("graph capture: a corrupt serialized probe yields HAZE_ERROR_INTERNAL, never terminate",
          "[integration]") {
    // Pin the program directory so the test can remove the ciphertext template
    // the launch would otherwise use to heal the planted corrupt probe. With no
    // template, probe reconstruction skips (a missing input is graceful, not a
    // parse error), the corrupt .ct survives into the program directory, and
    // fhetch::result() deserializes it — exercising the result-path cereal guard.
    std::error_code ec;
    const std::filesystem::path program_dir =
        std::filesystem::temp_directory_path(ec) / "haze_qa_issue3_corrupt_probe";
    std::filesystem::remove_all(program_dir, ec);
    const uint64_t modulus = setup_compute_config_in_dir(program_dir);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x3333ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x4444ULL, kRingDim);
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

    const std::set<std::filesystem::path> before = list_exec_dirs();
    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);
    const std::set<std::filesystem::path> after = list_exec_dirs();

    std::filesystem::path exec_dir;
    std::size_t new_count = 0;
    for (const std::filesystem::path &d : after) {
        if (!before.contains(d)) {
            exec_dir = d;
            ++new_count;
        }
    }
    REQUIRE(new_count == 1);
    REQUIRE(!exec_dir.empty());

    // Remove the ciphertext template from the exec's snapshot directory (the
    // source the launch rebuilds the program directory from). The launch wipes
    // and re-copies the program directory from the snapshot, so once the
    // template is absent from the snapshot the rebuilt program directory has no
    // template either; reconstruction then has nothing to rebuild the probe from
    // and skips, leaving the planted corrupt probe for fhetch::result() to read.
    // The program-directory copy is removed too as belt-and-suspenders (the
    // remove_all rebuild would clear it regardless).
    std::size_t removed = 0;
    for (const std::filesystem::path &tpl_dir :
         {exec_dir / "ciphertext_templates", program_dir / "ciphertext_templates"}) {
        if (std::filesystem::exists(tpl_dir, ec))
            removed += static_cast<std::size_t>(std::filesystem::remove_all(tpl_dir, ec));
    }
    REQUIRE(removed >= 1);
    // Plant a truncated/garbage serialized probe in the exec snapshot; the launch
    // copy carries it into the program directory, where reconstruction leaves it
    // untouched and fhetch::result() deserializes it.
    REQUIRE(clobber_file(exec_dir / "serialized_probes" / "haze_out_0.ct"));

    // A corrupt probe read is contained and mapped to HAZE_ERROR_INTERNAL; no
    // cereal::Exception reaches std::terminate across the noexcept C ABI.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    gg.release();

    // Tidy the pinned program directory; the next case resets to the default.
    std::filesystem::remove_all(program_dir, ec);
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

// ---------------------------------------------------------------------------
// [integration] Graph filesystem-integrity regressions.
//
// P12-GRAPH-01: an exec must replay ITS graph, or fail closed -- never silently
// replay whatever trace happens to sit in the shared program directory.
// P7-GRAPH-SEC-01: the launch's program-directory rebuild must not follow a
// symlink planted in that directory and overwrite an out-of-tree file.
// Both are rooted in replay_snapshot_locked's Step 3 (src/core/epoch.cpp).
// ---------------------------------------------------------------------------

TEST_CASE("graph capture: launch fails closed when the exec's private trace is gone instead of "
          "replaying a different graph left in the program directory",
          "[integration]") {
    // The exec captures dst = a + b and privately snapshots that trace. A SECOND
    // graph (dst = a - b) is then captured, rewriting the shared program
    // directory's sticky trace to the subtract op sequence. If the exec's own
    // trace is deleted, a launch must NOT fall back to the subtract graph left
    // in the program directory and silently compute the wrong function -- it
    // must fail closed with a representable error and leave the output
    // unmaterialized.
    std::error_code ec;
    const std::filesystem::path program_dir =
        std::filesystem::temp_directory_path(ec) / "haze_qa_p12_missing_trace";
    std::filesystem::remove_all(program_dir, ec);
    const uint64_t modulus = setup_compute_config_in_dir(program_dir);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x5151ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x6262ULL, kRingDim);
    REQUIRE(hazeMemcpy(a, avec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bvec.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // g1: dst = a + b, then instantiate an exec that privately snapshots it.
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

    const std::set<std::filesystem::path> before = list_exec_dirs();
    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, g1) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);
    const std::set<std::filesystem::path> after = list_exec_dirs();

    std::filesystem::path exec_dir;
    std::size_t new_count = 0;
    for (const std::filesystem::path &d : after) {
        if (!before.contains(d)) {
            exec_dir = d;
            ++new_count;
        }
    }
    REQUIRE(new_count == 1);
    REQUIRE(!exec_dir.empty());

    // g2: dst = a - b. Its EndCapture rewrites the shared program directory's
    // sticky trace to the subtract sequence -- the "different graph" a broken
    // fallback would run.
    hazeGraph_t g2 = nullptr;
    {
        CaptureGuard capture_drain;
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeSub(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &g2) == HAZE_SUCCESS);
    }
    REQUIRE(g2 != nullptr);
    GraphGuard gg2(g2);

    // Delete the exec's private trace(s): it can no longer prove what it replays.
    std::size_t removed = 0;
    for (std::filesystem::directory_iterator it(exec_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension() == ".fhetch") {
            std::filesystem::remove(it->path(), ec);
            ++removed;
        }
    }
    REQUIRE(removed >= 1);

    // Fail closed: the launch refuses rather than replaying g2's subtract.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INTERNAL);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);

    // No stale replay materialized the tagged output: a device->host read still
    // reports it not-flushed rather than returning subtract (or any) data.
    std::vector<uint64_t> out(kRingDim, 0xDEADBEEFULL);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();

    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    ge.release();
    REQUIRE(hazeGraphDestroy(g1) == HAZE_SUCCESS);
    gg1.release();
    REQUIRE(hazeGraphDestroy(g2) == HAZE_SUCCESS);
    gg2.release();
    std::filesystem::remove_all(program_dir, ec);
}

TEST_CASE("graph capture: launch does not follow a symlink planted in the program directory to "
          "overwrite an out-of-tree file",
          "[integration]") {
    // A symlink planted inside the shared program directory must never be
    // followed during the launch's program-directory rebuild: following it would
    // let an attacker redirect a trace/artifact write to an arbitrary path
    // outside the tree. The rebuild remove_all()s the program directory
    // (unlinking the planted symlink AS A LINK, never following it to its
    // target) and re-copies the trusted snapshot with skip_symlinks, so the
    // out-of-tree target is left untouched and the replay still runs correctly.
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "haze_qa_sec01_graph_symlink";
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path program_dir = root / "program";
    std::filesystem::create_directories(program_dir, ec);
    REQUIRE(!ec);

    // An out-of-tree victim file and a symlink to it inside the program dir.
    const std::filesystem::path victim = root / "victim.txt";
    {
        std::ofstream v(victim);
        v << "SENTINEL_BEFORE";
    }
    std::filesystem::create_symlink(victim, program_dir / "victim-link", ec);
    REQUIRE(!ec);

    const uint64_t modulus = setup_compute_config_in_dir(program_dir);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gdst(dst);

    const std::vector<uint64_t> avec = haze::test::make_residue(modulus, 0x7373ULL, kRingDim);
    const std::vector<uint64_t> bvec = haze::test::make_residue(modulus, 0x8484ULL, kRingDim);
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

    // Change the victim AFTER capture; a correct launch must not touch it.
    {
        std::ofstream v(victim, std::ios::trunc);
        v << "SENTINEL_AFTER_CAPTURE";
    }

    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);

    // The out-of-tree victim is preserved exactly (not clobbered through the
    // symlink), and it is still a regular file, not replaced by a copied trace.
    REQUIRE(std::filesystem::is_regular_file(victim, ec));
    std::string victim_content;
    {
        std::ifstream vin(victim);
        std::getline(vin, victim_content);
    }
    REQUIRE(victim_content == "SENTINEL_AFTER_CAPTURE");

    // The launch still replayed the captured add correctly.
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
    std::filesystem::remove_all(root, ec);
}
