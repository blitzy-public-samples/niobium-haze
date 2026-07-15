// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

// Backfill coverage for the stream and event lifecycle public API:
// hazeStreamCreateWithPriority, hazeEventCreateWithFlags, hazeEventRecord,
// and single-handle create/destroy/record round-trips with null-argument
// validation.

// ---------------------------------------------------------------------------
// Stream lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("stream/event lifecycle: a single stream can be created and destroyed", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    REQUIRE(s != nullptr);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("stream/event lifecycle: stream creation rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamCreate(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("stream/event lifecycle: a stream with priority can be created and destroyed", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreateWithPriority(&s, 0U, 0) == HAZE_SUCCESS);
    REQUIRE(s != nullptr);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("stream/event lifecycle: streams with different priorities can be created", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t high = nullptr;
    hazeStream_t low = nullptr;
    REQUIRE(hazeStreamCreateWithPriority(&high, 0U, -1) == HAZE_SUCCESS);
    REQUIRE(high != nullptr);
    REQUIRE(hazeStreamCreateWithPriority(&low, 0U, 0) == HAZE_SUCCESS);
    REQUIRE(low != nullptr);
    REQUIRE(high != low);
    REQUIRE(hazeStreamDestroy(high) == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(low) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Event lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("stream/event lifecycle: a single event can be created and destroyed", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(e != nullptr);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
}

TEST_CASE("stream/event lifecycle: event creation rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeEventCreate(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("stream/event lifecycle: an event with flags can be created and destroyed", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreateWithFlags(&e, 0U) == HAZE_SUCCESS);
    REQUIRE(e != nullptr);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Event record
// ---------------------------------------------------------------------------

TEST_CASE("stream/event lifecycle: an event records onto a stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeEventRecord(e, s) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
    REQUIRE(hazeStreamDestroy(s) == HAZE_SUCCESS);
}

TEST_CASE("stream/event lifecycle: an event records onto the default stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    REQUIRE(hazeEventRecord(e, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeEventDestroy(e) == HAZE_SUCCESS);
}
