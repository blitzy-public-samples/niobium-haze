// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// Configuration-error hardening for the public config API. Each negative
// case is paired with a positive control so the config surface is shown to
// both accept well-formed input and reject malformed input with the correct
// hazeError_t. Companion to test_config.cpp: every TEST_CASE name here is
// prefixed "config error: " so none collide with that file's cases. Every
// case exercises only set-time validation and the hazeConfigureDevice()
// ordering precondition, so none need the simulator and all are [unit].

// Ring-dimension errors.

TEST_CASE("config error: an unsupported ring dimension is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4095) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("config error: a zero ring dimension is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("config error: a supported ring dimension is accepted", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
}

// Ciphertext-modulus errors.

TEST_CASE("config error: a negative modulus index is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(-1, kQ0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("config error: an out-of-range modulus index is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(64, kQ0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("config error: a zero modulus value is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, 0ULL) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

// Device-configuration ordering errors.

TEST_CASE("config error: configuring the device before setting a ring dimension is rejected",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_CONFIGERR);
}

TEST_CASE("config error: a full valid configuration sequence succeeds", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    constexpr uint64_t kQ0 = 576460752303415297ULL;
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}
