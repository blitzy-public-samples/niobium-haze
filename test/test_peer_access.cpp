// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Peer-access API (hazeDeviceCanAccessPeer, hazeDeviceEnablePeerAccess,
// hazeMemcpyPeerAsync) exercised against the in-process simulator, which
// exposes a single device (hazeGetDeviceCount == 1).
//
// Contract asserted here (single-device simulator):
//   query   - hazeDeviceCanAccessPeer zeroes *can on entry; a device is never
//             its own peer (0,0 -> SUCCESS, can == 0); negative or out-of-range
//             device/peer ordinals -> HAZE_ERROR_INVALID_VALUE; null out-pointer
//             -> HAZE_ERROR_INVALID_VALUE.
//   enable  - hazeDeviceEnablePeerAccess rejects non-zero flags, the active
//             device as its own peer, and negative/out-of-range peers. With a
//             single device there is no distinct peer to authorize, so peer
//             access can never be enabled; the enabled state is reachable only
//             on multi-device hardware (see the [.][hardware] case).
//   copy    - hazeMemcpyPeerAsync validates arguments in the order
//             null-args -> device ordinals -> pointer lookup -> source
//             availability: null dst/src or negative/out-of-range ordinals ->
//             HAZE_ERROR_INVALID_VALUE; an unmapped device address ->
//             HAZE_ERROR_UNKNOWN_ADDRESS; an allocated-but-never-written source
//             -> HAZE_ERROR_SOURCE_UNAVAILABLE. A degenerate same-device copy
//             (dst_device == src_device == 0) records a device-to-device copy
//             that needs no peer authorization and round-trips bytes through
//             tag/flush/D2H. The stream is accepted for CUDA-shape parity.
//
// Cases that need genuinely distinct physical peer hardware are hidden behind
// the "[.][hardware]" tag so the default suite stays green without hardware;
// physical multi-chip validation is human follow-up.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

namespace {

constexpr std::size_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Scope guard that frees a hazeMalloc'd device pointer on scope exit so a
// REQUIRE failure mid-case cannot leak the allocation. release() hands
// ownership back when the test frees explicitly.
class DeviceGuard {
  public:
    explicit DeviceGuard(void *ptr) noexcept : ptr_(ptr) {}
    DeviceGuard(const DeviceGuard &) = delete;
    DeviceGuard &operator=(const DeviceGuard &) = delete;
    DeviceGuard(DeviceGuard &&) = delete;
    DeviceGuard &operator=(DeviceGuard &&) = delete;
    ~DeviceGuard() {
        if (ptr_ != nullptr)
            (void)hazeFree(ptr_);
    }
    void *get() const noexcept { return ptr_; }
    void *release() noexcept {
        void *p = ptr_;
        ptr_ = nullptr;
        return p;
    }

  private:
    void *ptr_ = nullptr;
};

// Scope guard that destroys a created stream on scope exit.
class StreamGuard {
  public:
    explicit StreamGuard(hazeStream_t stream) noexcept : stream_(stream) {}
    StreamGuard(const StreamGuard &) = delete;
    StreamGuard &operator=(const StreamGuard &) = delete;
    StreamGuard(StreamGuard &&) = delete;
    StreamGuard &operator=(StreamGuard &&) = delete;
    ~StreamGuard() {
        if (stream_ != nullptr)
            (void)hazeStreamDestroy(stream_);
    }
    hazeStream_t get() const noexcept { return stream_; }

  private:
    hazeStream_t stream_ = nullptr;
};

// Reset + ring dimension + configure, enough to hazeMalloc device buffers
// without a full crypto context. Used by the argument-validation cases that
// fail before any source materialization.
void configure_single_device() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

} // namespace

// ---------------------------------------------------------------------------
// hazeDeviceCanAccessPeer (query stage)
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
    REQUIRE(can == 0);
    hazeGetLastError();
}

TEST_CASE("peer access: canAccessPeer rejects negative ordinals", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int can = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can, -1, 0) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(can == 0);
    hazeGetLastError();
    can = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 0, -1) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(can == 0);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// hazeDeviceEnablePeerAccess (enable stage)
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

TEST_CASE("peer access: enablePeerAccess rejects a negative peer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceEnablePeerAccess(-1, 0U) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: peer access cannot be enabled on the single-device simulator", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int count = 0;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    REQUIRE(count == 1);

    // With one device there is no distinct peer to authorize: every in-range
    // enable attempt is rejected, so peer access stays disabled and the query
    // surface confirms the device is never its own peer. The enabled state is
    // exercised only by the [.][hardware] case below.
    REQUIRE(hazeDeviceEnablePeerAccess(0, 0U) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    int can = -1;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 0, 0) == HAZE_SUCCESS);
    REQUIRE(can == 0);
}

// ---------------------------------------------------------------------------
// hazeMemcpyPeerAsync (argument validation)
// ---------------------------------------------------------------------------

TEST_CASE("peer access: memcpyPeerAsync rejects a null destination", "[unit]") {
    configure_single_device();
    void *src = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_guard(src);

    REQUIRE(hazeMemcpyPeerAsync(nullptr, 0, src, 0, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: memcpyPeerAsync rejects a null source", "[unit]") {
    configure_single_device();
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_guard(dst);

    REQUIRE(hazeMemcpyPeerAsync(dst, 0, nullptr, 0, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: memcpyPeerAsync validates device ordinals", "[unit]") {
    configure_single_device();
    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_guard(src);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_guard(dst);

    // Negative and out-of-range ordinals are rejected before any pointer
    // lookup. On the single-device simulator only ordinal 0 is in range.
    REQUIRE(hazeMemcpyPeerAsync(dst, -1, src, 0, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeMemcpyPeerAsync(dst, 0, src, -1, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeMemcpyPeerAsync(dst, 1, src, 0, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
    REQUIRE(hazeMemcpyPeerAsync(dst, 0, src, 1, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("peer access: memcpyPeerAsync rejects an unknown device address", "[unit]") {
    configure_single_device();
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_guard(dst);

    // A synthetic device address in the HBM range that was never allocated is
    // classified as unmapped. The int-to-ptr cast is deliberate: the test
    // needs the address itself, not a real allocation.
    // NOLINTBEGIN(performance-no-int-to-ptr)
    void *unknown_src = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0xA000000ULL);
    // NOLINTEND(performance-no-int-to-ptr)
    REQUIRE(hazeMemcpyPeerAsync(dst, 0, unknown_src, 0, kBytes, nullptr) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// hazeMemcpyPeerAsync (degenerate same-device copy on the simulator)
// ---------------------------------------------------------------------------

TEST_CASE("peer access: memcpyPeerAsync round-trips a compute-produced polynomial",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    void *src_in = nullptr;
    void *dst_compute = nullptr;
    void *dst_peer = nullptr;
    REQUIRE(hazeMalloc(&src_in, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_in_guard(src_in);
    REQUIRE(hazeMalloc(&dst_compute, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_compute_guard(dst_compute);
    REQUIRE(hazeMalloc(&dst_peer, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_peer_guard(dst_peer);

    const auto residue = haze::test::make_residue(q, /*seed=*/42, kRingDim);
    REQUIRE(hazeMemcpy(src_in, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // Compute-produced source: dst_compute = src_in + src_in is bound in
    // poly_map_ with no shadow_data_ entry until flush.
    REQUIRE(hazeAdd(dst_compute, src_in, src_in, 0, nullptr) == HAZE_SUCCESS);

    // Degenerate same-device peer copy (dst_device == src_device == 0) records
    // a device-to-device copy on the default stream (nullptr).
    REQUIRE(hazeMemcpyPeerAsync(dst_peer, 0, dst_compute, 0, kBytes, nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeTagOutput(dst_peer) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), dst_peer, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t i = 0; i < kRingDim; ++i)
        REQUIRE(out[i] == (residue[i] + residue[i]) % q);
}

TEST_CASE("peer access: memcpyPeerAsync accepts a non-default stream for parity", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config();

    void *src_in = nullptr;
    void *dst_compute = nullptr;
    void *dst_peer = nullptr;
    REQUIRE(hazeMalloc(&src_in, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_in_guard(src_in);
    REQUIRE(hazeMalloc(&dst_compute, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_compute_guard(dst_compute);
    REQUIRE(hazeMalloc(&dst_peer, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_peer_guard(dst_peer);

    const auto residue = haze::test::make_residue(q, /*seed=*/7, kRingDim);
    REQUIRE(hazeMemcpy(src_in, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst_compute, src_in, src_in, 0, nullptr) == HAZE_SUCCESS);

    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);

    // The stream is accepted for CUDA-shape parity; ordering is not honoured,
    // so the recorded copy materializes identically to the default-stream case.
    REQUIRE(hazeMemcpyPeerAsync(dst_peer, 0, dst_compute, 0, kBytes, stream) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst_peer) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), dst_peer, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t i = 0; i < kRingDim; ++i)
        REQUIRE(out[i] == (residue[i] + residue[i]) % q);
}

TEST_CASE("peer access: memcpyPeerAsync of a never-written source returns SOURCE_UNAVAILABLE",
          "[integration]") {
    haze::test::setup_integration_compute_config();

    void *src = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&src, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_guard(src);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_guard(dst);

    // src is allocated but never H2D'd or compute-touched: the peer copy must
    // fail loudly rather than silently produce a zero-filled dst.
    REQUIRE(hazeMemcpyPeerAsync(dst, 0, src, 0, kBytes, nullptr) == HAZE_ERROR_SOURCE_UNAVAILABLE);
    hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Hardware-gated multi-device contract (hidden by default; opt in with
// `./haze_tests "[hardware]"`). Physical multi-chip validation is human
// follow-up and is not gated by CI.
// ---------------------------------------------------------------------------

TEST_CASE("peer access: multi-device peer enable, copy, and readback", "[.][hardware]") {
    // setup_integration_compute_config resets device state first; the device
    // count is queried after so the case self-skips on the single-device
    // simulator without failing.
    const uint64_t q = haze::test::setup_integration_compute_config();

    int count = 0;
    REQUIRE(hazeGetDeviceCount(&count) == HAZE_SUCCESS);
    if (count < 2) {
        SUCCEED("requires >=2 devices; physical multi-chip validation is human follow-up");
        return;
    }

    // Enabled state: authorize access from the active device (0) to peer 1.
    int can = 0;
    REQUIRE(hazeDeviceCanAccessPeer(&can, 0, 1) == HAZE_SUCCESS);
    REQUIRE(can == 1);
    REQUIRE(hazeDeviceEnablePeerAccess(1, 0U) == HAZE_SUCCESS);

    // Allocate the source on device 1 and the destination on device 0. Precise
    // cross-device configuration semantics are human follow-up.
    REQUIRE(hazeSetDevice(1) == HAZE_SUCCESS);
    void *src_in = nullptr;
    void *dst_compute = nullptr;
    REQUIRE(hazeMalloc(&src_in, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_in_guard(src_in);
    REQUIRE(hazeMalloc(&dst_compute, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_compute_guard(dst_compute);

    const auto residue = haze::test::make_residue(q, /*seed=*/5, kRingDim);
    REQUIRE(hazeMemcpy(src_in, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst_compute, src_in, src_in, 0, nullptr) == HAZE_SUCCESS);

    REQUIRE(hazeSetDevice(0) == HAZE_SUCCESS);
    void *dst_peer = nullptr;
    REQUIRE(hazeMalloc(&dst_peer, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_peer_guard(dst_peer);

    // Cross-device peer copy from device 1 to device 0, then read back and
    // byte-compare against the expected 2 * residue mod q.
    REQUIRE(hazeMemcpyPeerAsync(dst_peer, 0, dst_compute, 1, kBytes, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst_peer) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), dst_peer, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);
    for (std::size_t i = 0; i < kRingDim; ++i)
        REQUIRE(out[i] == (residue[i] + residue[i]) % q);
}
