// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "common/errors.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string_view>
#include <thread>

TEST_CASE("error semantics: the last error clears after being read", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(nullptr, 32768) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("error semantics: the last error register is thread-local", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetDeviceProperties(nullptr, 0) == HAZE_ERROR_INVALID_VALUE);

    bool other_thread_saw_clean = false;
    std::thread t([&other_thread_saw_clean] {
        other_thread_saw_clean = (hazeGetLastError() == HAZE_SUCCESS);
    });
    t.join();

    REQUIRE(other_thread_saw_clean);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("error semantics: every public error code maps to a non-null string", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeError_t codes[] = {
        HAZE_SUCCESS,
        HAZE_ERROR_INVALID_VALUE,
        HAZE_ERROR_OUT_OF_MEMORY,
        HAZE_ERROR_NOT_SUPPORTED,
        HAZE_ERROR_CONFIGERR,
        HAZE_ERROR_UNKNOWN_ADDRESS,
        HAZE_ERROR_NO_DATA,
        HAZE_ERROR_ALLOC_TOO_SMALL,
        HAZE_ERROR_SOURCE_UNAVAILABLE,
        HAZE_ERROR_NOT_FLUSHED,
        HAZE_ERROR_INTERNAL,
    };
    for (const hazeError_t code : codes) {
        const char *message = hazeGetErrorString(code);
        REQUIRE(message != nullptr);
        REQUIRE(!std::string_view(message).empty());
    }
}

TEST_CASE("error semantics: an unknown error code yields a stable non-null fallback", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int raw = 999;
    hazeError_t bogus{};
    static_assert(sizeof(hazeError_t) == sizeof(raw), "enum size mismatch");
    std::memcpy(&bogus, &raw, sizeof(bogus));
    const char *message = hazeGetErrorString(bogus);
    REQUIRE(message != nullptr);
    REQUIRE(std::string_view(message) == "unknown error");
}

TEST_CASE("error semantics: every internal error variant maps to a defined public code", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    using haze::HazeInternalError;
    const HazeInternalError variants[] = {
        HazeInternalError::InvalidArgument,
        HazeInternalError::NotConfigured,
        HazeInternalError::UnknownAddress,
        HazeInternalError::NoData,
        HazeInternalError::AllocTooSmall,
        HazeInternalError::BackendInitFailed,
        HazeInternalError::BackendReplayFailed,
        HazeInternalError::BackendShapeMismatch,
        HazeInternalError::MrpGroupAddrModuliMismatch,
        HazeInternalError::MissingPolyMapBinding,
        HazeInternalError::ShadowSizeMismatch,
        HazeInternalError::BackendOutputMissing,
        HazeInternalError::BackendOutputDecodeFailed,
        HazeInternalError::BridgeHookFailed,
        HazeInternalError::PoolMapDesync,
        HazeInternalError::SourceUnavailable,
        HazeInternalError::OutputNotFlushed,
        HazeInternalError::UnsupportedDataFormat,
    };
    static_assert(sizeof(variants) / sizeof(variants[0]) == 18U,
                  "all 18 HazeInternalError variants must be enumerated");
    for (const HazeInternalError variant : variants) {
        const hazeError_t pub = haze::to_public_error(variant);
        REQUIRE(hazeGetErrorString(pub) != nullptr);
        REQUIRE((pub == HAZE_SUCCESS || pub == HAZE_ERROR_INVALID_VALUE ||
                 pub == HAZE_ERROR_OUT_OF_MEMORY || pub == HAZE_ERROR_NOT_SUPPORTED ||
                 pub == HAZE_ERROR_CONFIGERR || pub == HAZE_ERROR_UNKNOWN_ADDRESS ||
                 pub == HAZE_ERROR_NO_DATA || pub == HAZE_ERROR_ALLOC_TOO_SMALL ||
                 pub == HAZE_ERROR_SOURCE_UNAVAILABLE || pub == HAZE_ERROR_NOT_FLUSHED ||
                 pub == HAZE_ERROR_INTERNAL));
    }
}

TEST_CASE("error semantics: invalid arguments never throw across the C ABI", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE_NOTHROW([] {
        (void)hazeMalloc(nullptr, 0);
        (void)hazeFree(nullptr);
        (void)hazeMemcpy(nullptr, nullptr, 0, HAZE_MEMCPY_HOST_TO_DEVICE);
        (void)hazeGetDeviceProperties(nullptr, 99);
        (void)hazeTagOutput(nullptr);
        (void)hazeGraphDestroy(nullptr);
    }());
    (void)hazeGetLastError();
}
