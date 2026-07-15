// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string_view>
#include <vector>

// Introspection surface of the public C ABI: what haze reports about error
// codes (hazeGetErrorString), pointers (hazePointerGetAttributes), and MRP
// residue-group allocations (hazeMallocMrp / hazeFreeMrp). These cases are
// deliberately named "introspection: ..." so they complement, rather than
// duplicate, the allocation-oriented cases in test_memory.cpp.

// ---------------------------------------------------------------------------
// hazeGetErrorString — every public code maps to a stable, non-null string.
// ---------------------------------------------------------------------------

TEST_CASE("introspection: every public error code has a non-null description", "[unit]") {
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
    for (hazeError_t code : codes) {
        const char *desc = hazeGetErrorString(code);
        REQUIRE(desc != nullptr);
        REQUIRE(!std::string_view(desc).empty());
    }
}

TEST_CASE("introspection: the success code maps to a descriptive string", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const char *desc = hazeGetErrorString(HAZE_SUCCESS);
    REQUIRE(desc != nullptr);
    REQUIRE(!std::string_view(desc).empty());
}

TEST_CASE("introspection: an out-of-range error code yields a non-null fallback", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Out-of-range enum value built via memcpy (avoids -Wconversion on a direct cast).
    int raw = 999;
    hazeError_t bogus{};
    static_assert(sizeof(hazeError_t) == sizeof(raw), "enum size mismatch");
    std::memcpy(&bogus, &raw, sizeof(bogus));
    REQUIRE(hazeGetErrorString(bogus) != nullptr);
}

// ---------------------------------------------------------------------------
// hazePointerGetAttributes — memory-type classification and null rejection.
// ---------------------------------------------------------------------------

TEST_CASE("introspection: an unregistered host pointer reports the unregistered type", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int stack_var = 0;
    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, &stack_var) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_UNREGISTERED);
}

TEST_CASE("introspection: pointer-attribute query rejects a null attributes struct", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int probe = 0;
    REQUIRE(hazePointerGetAttributes(nullptr, &probe) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// hazeMallocMrp / hazeFreeMrp — residue-group allocation introspection.
// ---------------------------------------------------------------------------

TEST_CASE("introspection: MRP allocation returns distinct residue pointers", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    constexpr size_t kCount = 3;
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    std::vector<void *> ptrs(kCount, nullptr);
    REQUIRE(hazeMallocMrp(ptrs.data(), kCount, kBytes) == HAZE_SUCCESS);
    for (size_t i = 0; i < kCount; ++i) {
        REQUIRE(ptrs[i] != nullptr);
        for (size_t j = i + 1; j < kCount; ++j)
            REQUIRE(ptrs[i] != ptrs[j]);
    }
    REQUIRE(hazeFreeMrp(ptrs.data(), kCount) == HAZE_SUCCESS);
}

TEST_CASE("introspection: MRP allocation rejects a null pointer array", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    constexpr size_t kBytes = 4096 * sizeof(uint64_t);
    REQUIRE(hazeMallocMrp(nullptr, 3, kBytes) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}
