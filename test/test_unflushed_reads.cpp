// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// P2e unflushed-read hardening. A device->host read of a recorded result
// returns HAZE_ERROR_NOT_FLUSHED until hazeTagOutput + hazeFlush materialize
// it; a plain host-to-device buffer instead reads back its uploaded bytes.
// Device pointers are opaque shadow handles and are never dereferenced here.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr uint64_t kModulus = 576460752303415297ULL;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

} // namespace

TEST_CASE("unflushed read: reading a recorded result before flush returns not-flushed",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kModulus, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> host_a = haze::test::make_residue(modulus, 1, kRingDim);
    const std::vector<uint64_t> host_b = haze::test::make_residue(modulus, 2, kRingDim);
    REQUIRE(hazeMemcpy(a, host_a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, host_b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_ERROR_NOT_FLUSHED);
    hazeGetLastError();

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

TEST_CASE("unflushed read: reading a recorded result after tag and flush succeeds",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kModulus, 0);

    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> host_a = haze::test::make_residue(modulus, 1, kRingDim);
    const std::vector<uint64_t> host_b = haze::test::make_residue(modulus, 2, kRingDim);
    REQUIRE(hazeMemcpy(a, host_a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, host_b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), dst, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t i = 0; i < kRingDim; ++i) {
        REQUIRE(out[i] == haze::test::add_mod(host_a[i], host_b[i], modulus));
    }

    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

TEST_CASE("unflushed read: a plain host-to-device buffer reads back without a flush",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const uint64_t modulus = haze::test::setup_integration_compute_config(kRingDim, kModulus, 0);

    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> host = haze::test::make_residue(modulus, 7, kRingDim);
    REQUIRE(hazeMemcpy(p, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim, 0);
    REQUIRE(hazeMemcpy(out.data(), p, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    REQUIRE(out == host);

    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
}

TEST_CASE("unflushed read: a device shadow pointer is not a dereferenceable host address",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    haze::test::setup_integration_compute_config(kRingDim, kModulus, 0);

    void *p = nullptr;
    REQUIRE(hazeMalloc(&p, kBytes) == HAZE_SUCCESS);

    hazePointerAttributes attrs{};
    REQUIRE(hazePointerGetAttributes(&attrs, p) == HAZE_SUCCESS);
    REQUIRE(attrs.type == HAZE_MEMORY_TYPE_DEVICE);

    REQUIRE(hazeFree(p) == HAZE_SUCCESS);
}
