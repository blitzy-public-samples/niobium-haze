// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "integration_helpers.hpp"

#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <memory>
#include <thread>
#include <vector>

// Error-path hardening for the memory / handle C ABI: invalid and null
// handles, use-after-free, double-free, and wrong-allocator misuse. Every case
// asserts only public return codes and never dereferences a freed or device
// (shadow) address from the host, so the whole TU stays clean under the
// HAZE_SANITIZERS (ASan+UBSan) and HAZE_TSAN builds. Device allocations are
// owned by a DeviceGuard so a failed REQUIRE frees them during unwinding.

namespace {

// Frees a device allocation at scope exit unless release() has been called.
class DeviceGuard {
  public:
    explicit DeviceGuard(void *ptr) noexcept : ptr_(ptr) {}
    DeviceGuard(const DeviceGuard &) = delete;
    DeviceGuard &operator=(const DeviceGuard &) = delete;
    DeviceGuard(DeviceGuard &&) = delete;
    DeviceGuard &operator=(DeviceGuard &&) = delete;
    ~DeviceGuard() {
        if (ptr_ != nullptr) {
            (void)hazeFree(ptr_);
        }
    }
    void release() noexcept { ptr_ = nullptr; }

  private:
    void *ptr_;
};

// Frees a host (pinned) allocation at scope exit via the matching deallocator.
class HostGuard {
  public:
    explicit HostGuard(void *ptr) noexcept : ptr_(ptr) {}
    HostGuard(const HostGuard &) = delete;
    HostGuard &operator=(const HostGuard &) = delete;
    HostGuard(HostGuard &&) = delete;
    HostGuard &operator=(HostGuard &&) = delete;
    ~HostGuard() {
        if (ptr_ != nullptr) {
            (void)hazeFreeHost(ptr_);
        }
    }

  private:
    void *ptr_;
};

// Destroys a graph handle at scope exit unless release() has been called, so a
// failed REQUIRE during graph-lifecycle setup cannot leak the on-disk snapshot.
class GraphGuard {
  public:
    explicit GraphGuard(hazeGraph_t graph) noexcept : graph_(graph) {}
    GraphGuard(const GraphGuard &) = delete;
    GraphGuard &operator=(const GraphGuard &) = delete;
    GraphGuard(GraphGuard &&) = delete;
    GraphGuard &operator=(GraphGuard &&) = delete;
    ~GraphGuard() {
        if (graph_ != nullptr) {
            (void)hazeGraphDestroy(graph_);
        }
    }
    void release() noexcept { graph_ = nullptr; }

  private:
    hazeGraph_t graph_;
};

// Destroys an exec handle at scope exit unless release() has been called.
class ExecGuard {
  public:
    explicit ExecGuard(hazeGraphExec_t exec) noexcept : exec_(exec) {}
    ExecGuard(const ExecGuard &) = delete;
    ExecGuard &operator=(const ExecGuard &) = delete;
    ExecGuard(ExecGuard &&) = delete;
    ExecGuard &operator=(ExecGuard &&) = delete;
    ~ExecGuard() {
        if (exec_ != nullptr) {
            (void)hazeGraphExecDestroy(exec_);
        }
    }
    void release() noexcept { exec_ = nullptr; }

  private:
    hazeGraphExec_t exec_;
};

} // namespace

// ---------------------------------------------------------------------------
// Null / invalid handle cases: pure argument validation, no device state.
// ---------------------------------------------------------------------------

TEST_CASE("error path: malloc rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(nullptr, 32768) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: freeing a null device pointer is a silent success", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // hazeFree(NULL) matches cudaFree(NULL): a documented no-op that succeeds
    // and leaves the thread-local last-error untouched.
    REQUIRE(hazeFree(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("error path: memcpy rejects two null pointers", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(nullptr, nullptr, 32768, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: memcpy rejects a null destination", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // The source buffer matches the byte count so the case exercises the null
    // destination rather than relying on an undersized buffer being ignored.
    constexpr std::size_t kN = 4096;
    constexpr std::size_t kBytes = kN * sizeof(uint64_t);
    const std::vector<uint64_t> src(kN, 0);
    REQUIRE(hazeMemcpy(nullptr, src.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: memcpy rejects a null source", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // The destination buffer matches the byte count so the case exercises the
    // null source rather than relying on an undersized buffer being ignored.
    constexpr std::size_t kN = 4096;
    constexpr std::size_t kBytes = kN * sizeof(uint64_t);
    std::vector<uint64_t> dst(kN, 0);
    REQUIRE(hazeMemcpy(dst.data(), nullptr, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: tagging a null output pointer is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: host alloc rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeHostAlloc(nullptr, 4096, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: host alloc rejects a zero size", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *h = nullptr;
    REQUIRE(hazeHostAlloc(&h, 0, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Use-after-free / double-free: the freed DevAddr leaves the allocator's
// tracked set, so a second reference is reported, never dereferenced.
// ---------------------------------------------------------------------------

TEST_CASE("error path: freeing the same device pointer twice is rejected", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);

    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    DeviceGuard guard(p);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    guard.release(); // p is freed; the second free below must be rejected, not repeated in the dtor
    REQUIRE(hazeFree(p) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
}

TEST_CASE("error path: reading a device pointer after free is rejected", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    constexpr std::size_t kN = 4096;
    constexpr std::size_t kBytes = kN * sizeof(uint64_t);

    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    DeviceGuard guard(p);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    guard.release();

    // The D2H lookup checks the tracked-address set before touching the host
    // buffer, so the freed address is rejected and the sink is left untouched.
    // Fill the sink with a sentinel and assert every word is unchanged.
    constexpr uint64_t kSentinel = 0xA5A5A5A5A5A5A5A5ULL;
    std::vector<uint64_t> host_sink(kN, kSentinel);
    REQUIRE(hazeMemcpy(host_sink.data(), p, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
    for (const uint64_t word : host_sink) {
        REQUIRE(word == kSentinel);
    }
}

// ---------------------------------------------------------------------------
// Wrong-allocator misuse: a host allocation is unknown to the device allocator.
// The host pointer is a real allocation, so freeing it via the matching
// hazeFreeHost afterwards is a valid cleanup and keeps the sanitizers quiet.
// ---------------------------------------------------------------------------

TEST_CASE("error path: freeing a host pointer with the device free is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *h = nullptr;
    REQUIRE(hazeHostAlloc(&h, 4096, 0) == HAZE_SUCCESS);
    REQUIRE(h != nullptr);
    // The guard releases h through the matching deallocator at scope exit, so
    // the rejected device-free assertion below cannot leak it.
    const HostGuard guard_h(h);

    // The device allocator never tracked this host address.
    REQUIRE(hazeFree(h) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Lock-order / concurrency stress, hidden by default via the [.] tag and
// intended for the HAZE_TSAN build. Many worker threads are released together
// by a std::barrier and then each drives an independent malloc -> memset ->
// free cycle on pointers it exclusively owns. This puts the device allocator's
// internal mutex (the lower node of the documented epoch -> allocator lock
// order) under maximum contention without any cross-thread pointer sharing, so
// a clean TSan run demonstrates the locking is well ordered. Concurrent op
// recording is outside the single-writer record-and-replay model and is not
// attempted here.
// ---------------------------------------------------------------------------

TEST_CASE("error path: concurrent allocate/memset/free is race-free under contention",
          "[.][concurrency]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    constexpr int kThreads = 8;
    constexpr int kIterations = 64;

    std::barrier start(kThreads);
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i) {
                void *p = nullptr;
                if (hazeMalloc(&p, kBytes) != HAZE_SUCCESS) {
                    failures.fetch_add(1);
                    continue;
                }
                if (hazeMemset(p, 0, kBytes) != HAZE_SUCCESS) {
                    failures.fetch_add(1);
                }
                if (hazeFree(p) != HAZE_SUCCESS) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    REQUIRE(failures.load() == 0);
}

// ---------------------------------------------------------------------------
// Graph-lifecycle concurrency, hidden by default via the [.] tag and intended
// for the HAZE_TSAN build. The record-and-replay model is single-writer for
// RECORDING, so every graph is captured sequentially on this thread first;
// only the replay-time operations (launch / update / destroy) are then
// exercised concurrently. These cases put the graph handle registry
// (g_handle_mutex), the epoch mutex the launch nests under, and the per-launch
// correlation-id generator and metrics counters under contention, so a clean
// TSan run demonstrates those paths are well ordered and race-free.
// ---------------------------------------------------------------------------

TEST_CASE("error path: concurrent launches of independent graph execs are race-free",
          "[.][concurrency]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(4096);
    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    constexpr int kThreads = 4;
    constexpr int kIterations = 16;

    // Shared, read-only operands: concurrent launches only ever read them.
    void *a = nullptr;
    void *b = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    const std::vector<uint64_t> av = haze::test::make_residue(modulus, 0x1234ULL, 4096);
    const std::vector<uint64_t> bv = haze::test::make_residue(modulus, 0x5678ULL, 4096);
    REQUIRE(hazeMemcpy(a, av.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bv.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // One fully independent (graph, exec) per thread, each writing to its own
    // output address, captured sequentially up front. RAII guards (owned via
    // unique_ptr so the non-movable guards can live in a vector) free every
    // resource even if a setup REQUIRE throws.
    std::vector<void *> dst(kThreads, nullptr);
    std::vector<hazeGraph_t> graphs(kThreads, nullptr);
    std::vector<hazeGraphExec_t> execs(kThreads, nullptr);
    std::vector<std::unique_ptr<DeviceGuard>> dst_guards;
    std::vector<std::unique_ptr<GraphGuard>> graph_guards;
    std::vector<std::unique_ptr<ExecGuard>> exec_guards;
    for (int t = 0; t < kThreads; ++t) {
        REQUIRE(hazeMalloc(&dst[static_cast<std::size_t>(t)], kBytes) == HAZE_SUCCESS);
        dst_guards.push_back(std::make_unique<DeviceGuard>(dst[static_cast<std::size_t>(t)]));
        REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeAdd(dst[static_cast<std::size_t>(t)], a, b, 0, nullptr) == HAZE_SUCCESS);
        REQUIRE(hazeTagOutput(dst[static_cast<std::size_t>(t)]) == HAZE_SUCCESS);
        REQUIRE(hazeStreamEndCapture(nullptr, &graphs[static_cast<std::size_t>(t)]) ==
                HAZE_SUCCESS);
        REQUIRE(graphs[static_cast<std::size_t>(t)] != nullptr);
        graph_guards.push_back(std::make_unique<GraphGuard>(graphs[static_cast<std::size_t>(t)]));
        REQUIRE(hazeGraphInstantiate(&execs[static_cast<std::size_t>(t)],
                                     graphs[static_cast<std::size_t>(t)]) == HAZE_SUCCESS);
        REQUIRE(execs[static_cast<std::size_t>(t)] != nullptr);
        exec_guards.push_back(std::make_unique<ExecGuard>(execs[static_cast<std::size_t>(t)]));
    }

    std::barrier start(kThreads);
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            start.arrive_and_wait();
            hazeGraphExec_t exec = execs[static_cast<std::size_t>(t)];
            for (int i = 0; i < kIterations; ++i) {
                if (hazeGraphLaunch(exec, nullptr) != HAZE_SUCCESS) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    REQUIRE(failures.load() == 0);
}

TEST_CASE("error path: concurrent launch, update, and destroy of a graph exec are race-free",
          "[.][concurrency]") {
    const uint64_t modulus = haze::test::setup_integration_compute_config(4096);
    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    constexpr int kLaunchers = 3;
    constexpr int kIterations = 24;

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    DeviceGuard ga(a);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    DeviceGuard gb(b);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard gd(dst);
    const std::vector<uint64_t> av = haze::test::make_residue(modulus, 0x2222ULL, 4096);
    const std::vector<uint64_t> bv = haze::test::make_residue(modulus, 0x3333ULL, 4096);
    REQUIRE(hazeMemcpy(a, av.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, bv.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // One graph shared by every worker; a launchable exec is instantiated from
    // it. Same-topology updates re-use the same graph as the update source.
    hazeGraph_t graph = nullptr;
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    REQUIRE(graph != nullptr);
    GraphGuard gg(graph);

    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);
    ExecGuard ge(exec);

    // Because the token registry serializes every registry op and a destroyed
    // token is never dereferenced (G6 ABA guard), each call must return a
    // DEFINED code -- SUCCESS while the exec lives, INVALID_VALUE once it has
    // been destroyed -- and never crash. `failures` counts any other code.
    constexpr int kThreads = kLaunchers + 2;
    std::barrier start(kThreads);
    std::atomic<int> failures{0};
    std::atomic<bool> destroyed{false};

    const auto is_defined = [](hazeError_t rc) {
        return rc == HAZE_SUCCESS || rc == HAZE_ERROR_INVALID_VALUE;
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(kThreads));
    for (int t = 0; t < kLaunchers; ++t) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i) {
                if (!is_defined(hazeGraphLaunch(exec, nullptr))) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    // Updater: repeatedly refresh the same-topology exec while it is launched.
    workers.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kIterations; ++i) {
            if (!is_defined(hazeGraphExecUpdate(exec, graph))) {
                failures.fetch_add(1);
            }
        }
    });
    // Destroyer: tear the exec down exactly once, partway through the run.
    workers.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kIterations / 2; ++i) {
            (void)hazeGraphLaunch(exec, nullptr);
        }
        if (hazeGraphExecDestroy(exec) == HAZE_SUCCESS) {
            destroyed.store(true);
        }
    });
    for (std::thread &worker : workers) {
        worker.join();
    }

    REQUIRE(failures.load() == 0);
    REQUIRE(destroyed.load());
    // The worker already destroyed the exec, so disarm the guard to avoid a
    // double-destroy (harmless INVALID_VALUE, but keeping the guard honest
    // avoids masking a real leak under the sanitizers).
    ge.release();
    // A launch after destruction is rejected via the token lookup, proving the
    // handle is gone rather than dereferenced.
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}
