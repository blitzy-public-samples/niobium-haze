// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// Error-path hardening for the memory / handle C ABI: invalid and null
// handles, use-after-free, double-free, and wrong-allocator misuse. Every case
// asserts only public return codes and never dereferences a freed or device
// (shadow) address from the host, so the whole TU stays clean under the
// HAZE_SANITIZERS (ASan+UBSan) and HAZE_TSAN builds.

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
    uint64_t src[1] = {};
    REQUIRE(hazeMemcpy(nullptr, src, 32768, HAZE_MEMCPY_HOST_TO_DEVICE) ==
            HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("error path: memcpy rejects a null source", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    uint64_t dst[1] = {};
    REQUIRE(hazeMemcpy(dst, nullptr, 32768, HAZE_MEMCPY_DEVICE_TO_HOST) ==
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
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);

    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
}

TEST_CASE("error path: reading a device pointer after free is rejected", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
    constexpr size_t kN = 4096;
    constexpr size_t kBytes = kN * sizeof(uint64_t);

    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeFree(p) == HAZE_SUCCESS);

    // The D2H lookup checks the tracked-address set before touching the host
    // buffer, so the freed address is rejected and host_sink is left untouched.
    uint64_t host_sink[kN] = {};
    REQUIRE(hazeMemcpy(host_sink, p, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
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

    // The device allocator never tracked this host address.
    REQUIRE(hazeFree(h) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();

    // Release it through the matching deallocator.
    REQUIRE(hazeFreeHost(h) == HAZE_SUCCESS);
}
