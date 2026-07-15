// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string_view>
#include <thread>

TEST_CASE("hazeGetLastError returns HAZE_SUCCESS by default", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("hazeGetLastError clears after read", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Force a genuine error: Montgomery + bit-reversal on the local target is
    // rejected at flush. (hazeStreamBeginCapture is implemented now and returns
    // HAZE_SUCCESS, so it can no longer be used to set an error here.)
    REQUIRE(hazeSetTarget("local") == HAZE_SUCCESS);
    REQUIRE(hazeSetMontgomery(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetBitReversal(1) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_ERROR_NOT_SUPPORTED);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_NOT_SUPPORTED);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("hazeGetErrorString returns \"unknown error\" for out-of-range codes") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Construct an out-of-range enum value via memcpy to avoid the
    // -Wconversion warning GCC emits on static_cast<hazeError_t>(999).
    int raw = 999;
    hazeError_t unknown{};
    static_assert(sizeof(hazeError_t) == sizeof(raw), "enum size mismatch");
    std::memcpy(&unknown, &raw, sizeof(unknown));
    REQUIRE(std::string_view(hazeGetErrorString(unknown)) == "unknown error");
}

TEST_CASE("hazeGetDeviceCount compiles and links", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int count = -1;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    REQUIRE(count == 1);
}

TEST_CASE("graph capture API is linked and validates its handles", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Graph capture is implemented; the entry points validate their handles.
    REQUIRE(hazeStreamEndCapture(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphDestroy(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeGraphExecDestroy(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    hazeGraphExec_t exec = reinterpret_cast<hazeGraphExec_t>(0x1);
    REQUIRE(hazeGraphInstantiate(&exec, nullptr) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(exec == nullptr);
    hazeGetLastError();
    REQUIRE(hazeGraphLaunch(nullptr, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access is unavailable on the single-device simulator", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int can_access = -1;
    // Only device 0 exists, so peer index 1 is out of range.
    REQUIRE(hazeDeviceCanAccessPeer(&can_access, 0, 1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(can_access == 0);
    hazeGetLastError();
    // No valid distinct peer exists on a single device.
    REQUIRE(hazeDeviceEnablePeerAccess(1, 0) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("successful stubs do not pollute error state", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Ensure a sequence of successful calls leaves hazeGetLastError as HAZE_SUCCESS.
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    void *ptr = nullptr;
    REQUIRE(hazeMalloc(&ptr, 32768) == HAZE_SUCCESS);
    REQUIRE(hazeFree(ptr) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

// CUDA exposes a thread-local error state via cudaGetLastError with
// clear-on-read semantics; HAZE mirrors that so FIDESlib's error-handling
// idioms port unchanged. Functions also return their error directly — the
// thread-local state is a convenience, not the primary error channel.
// Reference: https://parallelprogrammer.substack.com/p/cuda-error-handling-a-definitive
TEST_CASE("error state is thread-local", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    // Set an error in the main thread via a null-argument validation failure.
    // (hazeStreamBeginCapture is implemented now and returns HAZE_SUCCESS; a
    // null-argument call only touches the thread-local last-error register, so
    // this test stays race-free under TSan.)
    REQUIRE(hazeGetDeviceCount(nullptr) == HAZE_ERROR_INVALID_VALUE);

    // A second thread has its own clean error state, and an error it sets must
    // not bleed back into the main thread.
    hazeError_t child_initial = HAZE_ERROR_INTERNAL;
    std::thread t([&child_initial] {
        child_initial = hazeGetLastError(); // child's own state: clean
        (void)hazeGetDeviceCount(nullptr);  // set an error in the child
    });
    t.join();

    REQUIRE(child_initial == HAZE_SUCCESS);                  // child saw a clean register
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE); // main's error survived
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);             // and clears on read
}
