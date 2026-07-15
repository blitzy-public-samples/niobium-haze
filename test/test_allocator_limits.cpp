// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "allocator_test_access.hpp"
#include "common/handle.hpp"
#include "core/allocator.hpp"
#include "core/device.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <set>
#include <vector>

// HAZE's device allocator serves a single polynomial size: every allocation
// is one polynomial of (ring_dim * sizeof(uint64_t)) bytes, configured via
// hazeSetRingDimension. Any other request size is rejected, an unconfigured
// ring dimension is a configuration error, and the bump-plus-recycle pool is
// uncapped, so device hazeMalloc never reports out-of-memory. These cases pin
// that size-validation contract and the free-list recycling behaviour.

namespace {
constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kPolyBytes = static_cast<std::size_t>(kRingDim) * sizeof(uint64_t);
} // namespace

TEST_CASE("allocator limit: malloc before ring dimension is configured reports a "
          "configuration error",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kPolyBytes) == HAZE_ERROR_CONFIGERR);
    hazeGetLastError();
}

TEST_CASE("allocator limit: a zero-byte allocation is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, 0) == HAZE_ERROR_ALLOC_TOO_SMALL);
    hazeGetLastError();
}

TEST_CASE("allocator limit: an undersized allocation is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    void *p = nullptr;
    // ring_dim=4096 -> polynomial bytes = 32768; a smaller request fails.
    REQUIRE(hazeMalloc(&p, 8192) == HAZE_ERROR_ALLOC_TOO_SMALL);
    hazeGetLastError();
}

TEST_CASE("allocator limit: an oversized allocation is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    void *p = nullptr;
    // The allocator demands the exact polynomial size, so a larger request
    // is rejected too (not just under-sized ones).
    REQUIRE(hazeMalloc(&p, 65536) == HAZE_ERROR_ALLOC_TOO_SMALL);
    hazeGetLastError();
}

TEST_CASE("allocator limit: the exact polynomial size is accepted", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kPolyBytes) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    // Device pointers live in the virtual HBM window based at kHbmBase.
    REQUIRE(haze::to_uintptr(haze::to_dev_addr(p)) >= haze::kHbmBase);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
}

TEST_CASE("allocator limit: a large batch of allocations all succeed and are distinct",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);

    // Plain hazeMalloc is bounded only by address space, not by the device's
    // ciphertext-modulus envelope that caps MRP groups; allocate well past it.
    constexpr std::size_t kBatch = 8 * static_cast<std::size_t>(haze::kMaxCiphertextModuli);

    std::vector<void *> ptrs;
    ptrs.reserve(kBatch);
    for (std::size_t i = 0; i < kBatch; ++i) {
        void *p = nullptr;
        REQUIRE(hazeMalloc(&p, kPolyBytes) == HAZE_SUCCESS);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }

    const std::set<void *> distinct(ptrs.begin(), ptrs.end());
    REQUIRE(distinct.size() == ptrs.size());

    for (void *p : ptrs) {
        REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    }
}

TEST_CASE("allocator limit: freed allocations are recycled from the free-list", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);

    const auto &alloc = haze::DeviceAllocator::instance();
    const std::size_t baseline = haze::test::AllocatorTestAccess::alloc_set_size(alloc);
    REQUIRE(baseline == 0);

    constexpr std::size_t kCount = 8;
    std::vector<void *> ptrs;
    ptrs.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        void *p = nullptr;
        REQUIRE(hazeMalloc(&p, kPolyBytes) == HAZE_SUCCESS);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == baseline + kCount);

    const std::set<void *> freed(ptrs.begin(), ptrs.end());
    for (void *p : ptrs) {
        REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    }
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == baseline);

    std::set<void *> reallocated;
    std::vector<void *> recycled;
    recycled.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        void *p = nullptr;
        REQUIRE(hazeMalloc(&p, kPolyBytes) == HAZE_SUCCESS);
        REQUIRE(p != nullptr);
        REQUIRE(freed.contains(p));
        reallocated.insert(p);
        recycled.push_back(p);
    }
    REQUIRE(reallocated == freed);
    REQUIRE(haze::test::AllocatorTestAccess::alloc_set_size(alloc) == baseline + kCount);

    for (void *p : recycled) {
        REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    }
}
