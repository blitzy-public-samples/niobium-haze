// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// Configuration-error hardening for the public config API. Each negative case
// is paired with a positive control so the config surface is shown to both
// accept well-formed input and reject malformed input with the correct
// hazeError_t. Companion to test_config.cpp: every TEST_CASE name here is
// prefixed "config error: " so none collide with that file's cases. Every case
// exercises only set-time validation and the hazeConfigureDevice() ordering
// precondition, so none need the simulator and all are [unit].
//
// Ciphertext-modulus contract exercised below (hazeSetCiphertextModulus):
//   - index < 0                                 -> HAZE_ERROR_INVALID_VALUE
//   - modulus == 0                              -> HAZE_ERROR_INVALID_VALUE
//   - index > current table size (a gap)        -> HAZE_ERROR_INVALID_VALUE
//   - index == current table size               -> append (HAZE_SUCCESS)
//   - index < current table size                -> overwrite (HAZE_SUCCESS)
//   - after hazeConfigureDevice(): same value   -> HAZE_SUCCESS
//   - after hazeConfigureDevice(): new value or
//     new index                                 -> HAZE_ERROR_CONFIGERR
// The table has no fixed maximum index: writes stay legal as long as they
// remain contiguous. The kMaxCiphertextModuli == 64 limit is a per-allocation
// group-size bound enforced by hazeMallocMrp, not a config-time index cap.

namespace {
constexpr uint64_t kQ0 = 576460752303415297ULL;
} // namespace

// ---------------------------------------------------------------------------
// Ring-dimension errors.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Ciphertext-modulus argument errors.
// ---------------------------------------------------------------------------

TEST_CASE("config error: a negative modulus index is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(-1, kQ0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("config error: a zero modulus value is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, 0ULL) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

// ---------------------------------------------------------------------------
// Ciphertext-modulus contiguity (sparse-write) boundary.
// ---------------------------------------------------------------------------

TEST_CASE("config error: a modulus write that skips the next contiguous slot is rejected",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    // Table is empty: the only legal index is 0. Writing index 1 (or any
    // higher index) leaves index 0 unset, which the sparse-write rule rejects.
    REQUIRE(hazeSetCiphertextModulus(1, kQ0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetCiphertextModulus(64, kQ0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);

    // Filling index 0 makes index 1 the next contiguous slot; index 2 is still
    // a gap.
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(2, kQ0 + 2) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeSetCiphertextModulus(1, kQ0 + 2) == HAZE_SUCCESS);
}

TEST_CASE("config error: an in-range modulus overwrite before configuration is accepted",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(hazeSetCiphertextModulus(i, kQ0 + (static_cast<uint64_t>(i) * 2)) == HAZE_SUCCESS);
    }
    // Index 1 already exists, so re-setting it to a new value overwrites in
    // place rather than being treated as a gap.
    REQUIRE(hazeSetCiphertextModulus(1, kQ0 + 100) == HAZE_SUCCESS);
}

TEST_CASE("config error: contiguous modulus writes are accepted up to and beyond index 63",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    // Populate the first 64 slots (indices 0..63) contiguously.
    for (int i = 0; i < 64; ++i) {
        REQUIRE(hazeSetCiphertextModulus(i, kQ0 + (static_cast<uint64_t>(i) * 2)) == HAZE_SUCCESS);
    }
    // Index 64 is the next contiguous slot, so it appends and succeeds: the
    // modulus table has no fixed maximum index. Index 65 then appends too.
    REQUIRE(hazeSetCiphertextModulus(64, kQ0 + 128) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(65, kQ0 + 130) == HAZE_SUCCESS);
    // A gap beyond the new end is still rejected.
    REQUIRE(hazeSetCiphertextModulus(67, kQ0 + 134) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}

TEST_CASE("config error: populated modulus slots remain usable and immutable after configuration",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(hazeSetCiphertextModulus(i, kQ0 + (static_cast<uint64_t>(i) * 2)) == HAZE_SUCCESS);
    }
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    // Every populated slot is still present: re-asserting its exact value is
    // accepted after configuration.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(hazeSetCiphertextModulus(i, kQ0 + (static_cast<uint64_t>(i) * 2)) == HAZE_SUCCESS);
    }
    // Post-configuration the table is immutable: changing a value or adding a
    // new index is rejected as a configuration error.
    REQUIRE(hazeSetCiphertextModulus(1, kQ0 + 500) == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeSetCiphertextModulus(4, kQ0 + 8) == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_CONFIGERR);
}

// ---------------------------------------------------------------------------
// Device-configuration ordering errors.
// ---------------------------------------------------------------------------

TEST_CASE("config error: configuring the device before setting a ring dimension is rejected",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_ERROR_CONFIGERR);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_CONFIGERR);
}

TEST_CASE("config error: a full valid configuration sequence succeeds", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ0) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}
