// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Backfill coverage for pinned host memory: hazeHostAlloc / hazeFreeHost.
// hazeHostAlloc returns real, directly-accessible page-aligned storage
// (posix_memalign under the hood), unlike the device shadow addresses handed
// out by hazeMalloc, so the buffer is written and read back here to prove it
// is genuine host storage. The flags argument is accepted for CUDA-shape
// parity; a nonzero value still yields writable, page-aligned memory.
//
// test_memory.cpp already covers a bare alloc/free round-trip and the HOST
// pointer-attribute report; every TEST_CASE below is prefixed "host memory: "
// to stay globally unique across the Catch2 suite.
//
// Negative coverage is limited to the well-defined paths: a null output
// pointer, a zero-size request, and freeing a null pointer. hazeFreeHost
// forwards to the libc allocator without a liveness registry, so freeing an
// unknown or already-freed pointer is undefined behaviour and is deliberately
// not exercised; a freed pointer is instead observed through the
// pointer-attribute query, which reclassifies it as unregistered.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <unistd.h>

namespace {

// Frees a host allocation on scope exit unless released, so a mid-case
// REQUIRE failure cannot leak it.
class HostGuard {
  public:
    explicit HostGuard(void *p) noexcept : p_(p) {}
    HostGuard(const HostGuard &) = delete;
    HostGuard &operator=(const HostGuard &) = delete;
    HostGuard(HostGuard &&) = delete;
    HostGuard &operator=(HostGuard &&) = delete;
    ~HostGuard() {
        if (p_ != nullptr)
            (void)hazeFreeHost(p_);
    }
    void *get() const noexcept { return p_; }
    void *release() noexcept {
        void *t = p_;
        p_ = nullptr;
        return t;
    }

  private:
    void *p_ = nullptr;
};

} // namespace

TEST_CASE("host memory: host allocation and free round-trip", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    HostGuard guard(p);
    REQUIRE(hazeFreeHost(guard.release()) == HAZE_SUCCESS);
}

TEST_CASE("host memory: host memory is directly writable and readable", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kBytes = 1024;
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, kBytes, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    HostGuard guard(p);

    std::memset(p, 0xAB, kBytes);
    auto *bytes = reinterpret_cast<std::uint8_t *>(p);
    REQUIRE(bytes[0] == 0xAB);
    REQUIRE(bytes[kBytes - 1] == 0xAB);
}

TEST_CASE("host memory: a nonzero flags value yields writable page-aligned storage", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kBytes = 4096;
    void *p = nullptr;
    // A nonzero flags value must not degrade the allocation: the returned
    // buffer is still page-aligned and fully writable/readable.
    REQUIRE(hazeHostAlloc(&p, kBytes, 1U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    HostGuard guard(p);

    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    REQUIRE(page > 0);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % page == 0);

    std::memset(p, 0x5A, kBytes);
    auto *bytes = reinterpret_cast<std::uint8_t *>(p);
    REQUIRE(bytes[0] == 0x5A);
    REQUIRE(bytes[kBytes - 1] == 0x5A);
}

TEST_CASE("host memory: host allocation rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeHostAlloc(nullptr, 4096, 0U) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("host memory: host allocation rejects a zero-size request", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 0, 0U) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(p == nullptr);
    (void)hazeGetLastError();
}

TEST_CASE("host memory: freeing a null host pointer is a safe no-op", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeFreeHost(nullptr) == HAZE_SUCCESS);
    (void)hazeGetLastError();
}

TEST_CASE("host memory: a host-allocated pointer classifies as host memory", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    HostGuard guard(p);

    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, p) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_HOST);
    REQUIRE(attr.hostPointer == p);
    REQUIRE(attr.devicePointer == nullptr);
    REQUIRE(attr.device == 0);
}

TEST_CASE("host memory: a freed host pointer reclassifies as unregistered", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    HostGuard guard(p);
    REQUIRE(hazeFreeHost(guard.release()) == HAZE_SUCCESS);

    // The pointer VALUE is inspected (never dereferenced): after free the host
    // registry no longer knows it, so it reports as unregistered.
    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, p) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_UNREGISTERED);
    REQUIRE(attr.hostPointer == nullptr);
    REQUIRE(attr.devicePointer == nullptr);
}
