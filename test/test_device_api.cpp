// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>           // IWYU pragma: keep
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// Device-management backfill: hazeSetDevice / hazeGetDevice are the
// device functions test_memory.cpp does not exercise. These cases cover
// the current-device get/set round-trip and out-of-range rejection on
// the single-device simulator, plus distinctly-named count/properties
// validation cases (null out-pointer, positive and negative out-of-range
// indices) that round out the device-management surface without colliding
// with test_memory.cpp. The negative-index properties case additionally
// asserts that the output struct is left untouched and the last-error
// register carries the rejection code.

// ---------------------------------------------------------------------------
// Current device: hazeGetDevice / hazeSetDevice.
// ---------------------------------------------------------------------------

TEST_CASE("device api: the default current device is zero", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int dev = -1;
    REQUIRE(hazeGetDevice(&dev) == HAZE_SUCCESS);
    REQUIRE(dev == 0);
}

TEST_CASE("device api: getDevice rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetDevice(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("device api: setting the current device to zero succeeds", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetDevice(0) == HAZE_SUCCESS);
    int dev = -1;
    REQUIRE(hazeGetDevice(&dev) == HAZE_SUCCESS);
    REQUIRE(dev == 0);
}

TEST_CASE("device api: setting an out-of-range device is rejected", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Only index 0 is valid on the single-device simulator (count == 1).
    REQUIRE(hazeSetDevice(1) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeSetDevice(-1) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Device count and properties (distinct names from test_memory.cpp).
// ---------------------------------------------------------------------------

TEST_CASE("device api: device count query rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetDeviceCount(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("device api: the simulator reports a single device", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int count = 0;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    REQUIRE(count == 1);
}

TEST_CASE("device api: device properties expose the simulator hardware profile", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeDeviceProp prop{};
    REQUIRE(hazeGetDeviceProperties(&prop, 0) == HAZE_SUCCESS);
    REQUIRE(prop.totalGlobalMem == 16ULL * 1024 * 1024 * 1024);
    REQUIRE(prop.maxCiphertextModuli == 64);
    REQUIRE(prop.numHBMBanks == 8);
    REQUIRE(prop.numRegisters == 64);
    REQUIRE(prop.numSupportedRingDims == 7);
}

TEST_CASE("device api: device properties query rejects a null struct pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetDeviceProperties(nullptr, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("device api: device properties query rejects an out-of-range device", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeDeviceProp prop{};
    REQUIRE(hazeGetDeviceProperties(&prop, 1) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("device api: device properties query rejects a negative device index", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeDeviceProp prop{};
    // Seed a sentinel field: a rejected query must not write the struct.
    prop.totalGlobalMem = 0xDEADBEEFULL;
    REQUIRE(hazeGetDeviceProperties(&prop, -1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(prop.totalGlobalMem == 0xDEADBEEFULL); // output untouched on failure
    // The rejection code is observable through the last-error register (and
    // cleared by this read).
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
}
