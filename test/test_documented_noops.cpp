// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

TEST_CASE("documented no-op: hazeDeviceSynchronize returns success", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamSynchronize returns success on the default stream",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(nullptr) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamSynchronize returns success on a real stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(s) == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamWaitEvent returns success", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeStreamWaitEvent(s, e, 0U) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamWaitEvent returns success on the default stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeStreamWaitEvent(nullptr, e, 0U) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: no-op calls leave the last-error register clean", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(nullptr) == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeStreamWaitEvent(nullptr, e, 0U) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}
