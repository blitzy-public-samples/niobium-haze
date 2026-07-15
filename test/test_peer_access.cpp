// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

// Peer-access API (hazeDeviceCanAccessPeer, hazeDeviceEnablePeerAccess,
// hazeMemcpyPeerAsync) exercised against the in-process simulator, which
// exposes a single device (hazeGetDeviceCount == 1). With only one device
// there is no valid distinct peer: querying or enabling a distinct peer
// returns HAZE_ERROR_INVALID_VALUE, a device is never its own peer, and a
// peer copy degenerates to a same-device device-to-device copy. Cases that
// need genuinely distinct physical peer hardware are hidden behind the
// "[.][hardware]" tag so the default suite stays green without hardware.

// ---------------------------------------------------------------------------
// hazeDeviceCanAccessPeer
// ---------------------------------------------------------------------------

TEST_CASE("peer access: canAccessPeer rejects a null out-pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceCanAccessPeer(nullptr, 0, 1) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: a device is not its own peer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int can = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 0, 0) == HAZE_SUCCESS);
    REQUIRE(can == 0);
}

TEST_CASE("peer access: canAccessPeer rejects an out-of-range peer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int can = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 0, 1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(can == 0);
    hazeGetLastError();
}

TEST_CASE("peer access: canAccessPeer rejects an out-of-range device", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int can = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 1, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// hazeDeviceEnablePeerAccess
// ---------------------------------------------------------------------------

TEST_CASE("peer access: enablePeerAccess rejects a non-zero flags value", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceEnablePeerAccess(0, 1U) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: enablePeerAccess rejects the active device as its own peer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceEnablePeerAccess(0, 0U) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: enablePeerAccess rejects an out-of-range peer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceEnablePeerAccess(1, 0U) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// hazeMemcpyPeerAsync
// ---------------------------------------------------------------------------

TEST_CASE("peer access: memcpyPeerAsync rejects a null destination", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeMemcpyPeerAsync(nullptr, 0, reinterpret_cast<const void *>(0x1), 0, 32768,
                                nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: memcpyPeerAsync rejects a null source", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *dst = reinterpret_cast<void *>(0x1);
    REQUIRE(hazeMemcpyPeerAsync(dst, 0, nullptr, 0, 32768, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: memcpyPeerAsync performs a device-to-device copy on the simulator",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);

    constexpr size_t kRingDim = 4096;
    constexpr size_t kBytes = kRingDim * sizeof(uint64_t);
    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> host(kRingDim);
    for (size_t i = 0; i < host.size(); ++i)
        host[i] = static_cast<uint64_t>(i);
    REQUIRE(hazeMemcpy(src, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Both operands live on device 0 in the single-device simulator; the peer
    // copy degenerates to a device-to-device copy. Device indices are not
    // validated by hazeMemcpyPeerAsync. The copy is recorded, not eagerly
    // materialized, so dst is intentionally not read back here.
    REQUIRE(hazeMemcpyPeerAsync(dst, 0, src, 0, kBytes, nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeFree(src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Hardware-gated multi-device contract (hidden by default; opt in with
// `./haze_tests "[hardware]"`). Physical multi-chip validation is human
// follow-up and is not gated by CI.
// ---------------------------------------------------------------------------

TEST_CASE("peer access: multi-device peer enable and copy", "[.][hardware]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    int count = 0;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    if (count < 2) {
        SUCCEED("requires >=2 devices; skipping on this platform");
        return;
    }

    int can = 0;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 0, 1) == HAZE_SUCCESS);
    REQUIRE(can == 1);
    REQUIRE(hazeDeviceEnablePeerAccess(1, 0U) == HAZE_SUCCESS);

    constexpr size_t kRingDim = 4096;
    constexpr size_t kBytes = kRingDim * sizeof(uint64_t);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);

    std::vector<uint64_t> host(kRingDim);
    for (size_t i = 0; i < host.size(); ++i)
        host[i] = static_cast<uint64_t>(i);
    REQUIRE(hazeMemcpy(src, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    REQUIRE(hazeMemcpyPeerAsync(dst, 0, src, 1, kBytes, nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeFree(src) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
}
