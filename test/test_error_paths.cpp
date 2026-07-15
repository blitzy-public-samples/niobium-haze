// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
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
