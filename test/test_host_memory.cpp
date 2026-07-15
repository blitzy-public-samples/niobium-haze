// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// hazeHostAlloc returns real, directly-accessible pinned host memory
// (page-aligned posix_memalign under the hood), unlike the device
// shadow addresses handed out by hazeMalloc — so the buffer can be
// written and read back here to prove it is genuine host storage.
// test_memory.cpp already covers a bare alloc/free round-trip and the
// HOST pointer-attribute report; every TEST_CASE below is prefixed
// "host memory: " to stay globally unique across the Catch2 suite.

TEST_CASE("host memory: host allocation and free round-trip", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFreeHost(p) == HAZE_SUCCESS);
}

TEST_CASE("host memory: host memory is directly writable and readable", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr size_t kBytes = 1024;
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, kBytes, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);

    std::memset(p, 0xAB, kBytes);
    auto *bytes = reinterpret_cast<std::uint8_t *>(p);
    REQUIRE(bytes[0] == 0xAB);
    REQUIRE(bytes[kBytes - 1] == 0xAB);

    REQUIRE(hazeFreeHost(p) == HAZE_SUCCESS);
}

TEST_CASE("host memory: host allocation honors a flags argument", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    // The flags parameter is accepted for CUDA-shape parity; a non-zero
    // value is honored (not rejected) by hazeHostAlloc.
    REQUIRE(hazeHostAlloc(&p, 4096, 1U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);
    REQUIRE(hazeFreeHost(p) == HAZE_SUCCESS);
}

TEST_CASE("host memory: host allocation rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeHostAlloc(nullptr, 4096, 0U) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("host memory: host allocation rejects a zero-size request", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 0, 0U) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(p == nullptr);
    hazeGetLastError();
}

TEST_CASE("host memory: freeing a null host pointer is a safe no-op", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeFreeHost(nullptr) == HAZE_SUCCESS);
}

TEST_CASE("host memory: a host-allocated pointer classifies as host memory", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *p = nullptr;
    REQUIRE(hazeHostAlloc(&p, 4096, 0U) == HAZE_SUCCESS);
    REQUIRE(p != nullptr);

    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, p) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_HOST);

    REQUIRE(hazeFreeHost(p) == HAZE_SUCCESS);
}
