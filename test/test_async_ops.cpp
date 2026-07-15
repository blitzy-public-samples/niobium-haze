// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// Async memory-transfer coverage: hazeMallocAsync, hazeFreeAsync,
// hazeMemcpyAsync, and hazeMemsetAsync exercised with explicit and
// default (null) streams, plus null-argument validation.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

// ---------------------------------------------------------------------------
// hazeMallocAsync / hazeFreeAsync — device allocation on an explicit stream.
// ---------------------------------------------------------------------------

TEST_CASE("async ops: async malloc and free round-trip with an explicit stream", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);

    void *p = nullptr;
    REQUIRE(hazeMallocAsync(&p, kBytes, s) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFreeAsync(p, s) == HAZE_SUCCESS);

    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("async ops: async malloc rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);

    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);

    REQUIRE(hazeMallocAsync(nullptr, kBytes, s) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// hazeMemcpyAsync — host<->device transfers on explicit and default streams.
// ---------------------------------------------------------------------------

TEST_CASE("async ops: async host-to-device copy succeeds with an explicit stream",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);

    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);

    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> host(kN, 3);
    REQUIRE(hazeMemcpyAsync(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, s) == HAZE_SUCCESS);

    std::vector<uint64_t> back(kN, 0);
    REQUIRE(hazeMemcpyAsync(back.data(), d, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST, s) == HAZE_SUCCESS);
    REQUIRE(back == host);

    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("async ops: async copy rejects a null destination", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);

    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);

    std::vector<uint64_t> host(kN, 0);
    REQUIRE(hazeMemcpyAsync(nullptr, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, s) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("async ops: async copy accepts the default (null) stream", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);

    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> host(kN, 7);
    REQUIRE(hazeMemcpyAsync(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, nullptr) ==
            HAZE_SUCCESS);

    std::vector<uint64_t> back(kN, 0);
    REQUIRE(hazeMemcpyAsync(back.data(), d, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST, nullptr) ==
            HAZE_SUCCESS);
    REQUIRE(back == host);

    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// hazeMemsetAsync — clears a device buffer on an explicit stream.
// ---------------------------------------------------------------------------

TEST_CASE("async ops: async memset on a device pointer succeeds with an explicit stream",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);

    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);

    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> seed(kN, 0xAB);
    REQUIRE(hazeMemcpyAsync(d, seed.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, s) == HAZE_SUCCESS);

    REQUIRE(hazeMemsetAsync(d, 0, kBytes, s) == HAZE_SUCCESS);

    std::vector<uint64_t> back(kN, 1);
    REQUIRE(hazeMemcpy(back.data(), d, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    const std::vector<uint64_t> zeros(kN, 0);
    REQUIRE(back == zeros);

    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("async ops: async memset rejects a null pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);

    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);

    REQUIRE(hazeMemsetAsync(nullptr, 0, kBytes, s) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();

    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}
