// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Backfill coverage for the asynchronous memory entry points: hazeMallocAsync,
// hazeFreeAsync, hazeMemcpyAsync, and hazeMemsetAsync. Each forwards to its
// synchronous counterpart and discards the stream argument, so the default
// (null) stream and an explicit stream produce identical results; both are
// exercised. The allocation and free paths are pure allocator operations and
// carry the [unit] tag; the copy paths move bytes through the shadow staging
// buffer and carry [integration].
//
// Negative coverage exercises each argument independently: a null output on
// malloc, a wrong allocation size, a null pointer / unknown pointer / double
// free on the free path, a null destination and a null source on the copy
// path, an unsupported copy kind, and a null / unknown / freed target and a
// wrong count on the memset path. A freed or unknown device address is
// rejected by the allocator (not undefined behaviour) because liveness is
// tracked in a set.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Frees a device pointer on scope exit unless released. release() transfers
// ownership back to the test when it frees the pointer itself.
class DeviceGuard {
  public:
    explicit DeviceGuard(void *p) noexcept : p_(p) {}
    DeviceGuard(const DeviceGuard &) = delete;
    DeviceGuard &operator=(const DeviceGuard &) = delete;
    DeviceGuard(DeviceGuard &&) = delete;
    DeviceGuard &operator=(DeviceGuard &&) = delete;
    ~DeviceGuard() {
        if (p_ != nullptr)
            (void)hazeFree(p_);
    }
    void *get() const noexcept { return p_; }
    void *release() noexcept {
        void *t = p_;
        p_ = nullptr;
        return t;
    }

  private:
    void *p_ = nullptr;
};

// Frees a stream on scope exit.
class StreamGuard {
  public:
    explicit StreamGuard(hazeStream_t s) noexcept : s_(s) {}
    StreamGuard(const StreamGuard &) = delete;
    StreamGuard &operator=(const StreamGuard &) = delete;
    StreamGuard(StreamGuard &&) = delete;
    StreamGuard &operator=(StreamGuard &&) = delete;
    ~StreamGuard() {
        if (s_ != nullptr)
            (void)hazeStreamDestroy(s_);
    }
    hazeStream_t get() const noexcept { return s_; }

  private:
    hazeStream_t s_ = nullptr;
};

// A device address that was never allocated. The allocator rejects it rather
// than dereferencing it.
void *unknown_device_ptr() noexcept {
    // NOLINTBEGIN(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0xB000000ULL);
    // NOLINTEND(performance-no-int-to-ptr)
}

// Ring dimension + device configuration: enough for hazeMalloc / hazeMemset /
// hazeMemcpy on device buffers without a crypto context.
void configure() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

} // namespace

// ---------------------------------------------------------------------------
// Async allocation / free ([unit]).
// ---------------------------------------------------------------------------

TEST_CASE("async ops: malloc and free round-trip on the default stream", "[unit]") {
    configure();
    void *ptr = nullptr;
    REQUIRE(hazeMallocAsync(&ptr, kBytes, nullptr) == HAZE_SUCCESS);
    REQUIRE(ptr != nullptr);
    DeviceGuard guard(ptr);
    REQUIRE(hazeFreeAsync(guard.release(), nullptr) == HAZE_SUCCESS);
}

TEST_CASE("async ops: malloc and free round-trip on an explicit stream", "[unit]") {
    configure();
    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);

    void *ptr = nullptr;
    REQUIRE(hazeMallocAsync(&ptr, kBytes, stream) == HAZE_SUCCESS);
    REQUIRE(ptr != nullptr);
    DeviceGuard guard(ptr);
    REQUIRE(hazeFreeAsync(guard.release(), stream) == HAZE_SUCCESS);
}

TEST_CASE("async ops: malloc rejects a null output pointer", "[unit]") {
    configure();
    REQUIRE(hazeMallocAsync(nullptr, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: malloc rejects a size other than the polynomial size", "[unit]") {
    configure();
    void *ptr = nullptr;
    REQUIRE(hazeMallocAsync(&ptr, kBytes + sizeof(uint64_t), nullptr) ==
            HAZE_ERROR_ALLOC_TOO_SMALL);
    REQUIRE(ptr == nullptr); // output left untouched on failure
    (void)hazeGetLastError();
}

TEST_CASE("async ops: free tolerates a null pointer", "[unit]") {
    configure();
    REQUIRE(hazeFreeAsync(nullptr, nullptr) == HAZE_SUCCESS);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: free rejects an unknown device pointer", "[unit]") {
    configure();
    REQUIRE(hazeFreeAsync(unknown_device_ptr(), nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: free rejects a double free", "[unit]") {
    configure();
    void *ptr = nullptr;
    REQUIRE(hazeMallocAsync(&ptr, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(ptr);
    REQUIRE(hazeFreeAsync(guard.release(), nullptr) == HAZE_SUCCESS);
    // The address is no longer live, so the second free is rejected.
    REQUIRE(hazeFreeAsync(ptr, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    (void)hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Async host-to-device / device-to-host copy ([integration]).
// ---------------------------------------------------------------------------

TEST_CASE("async ops: host-to-device copy then device-to-host readback round-trips",
          "[integration]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMallocAsync(&device, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(device);

    std::vector<uint64_t> host_in(kRingDim);
    for (std::size_t i = 0; i < kRingDim; ++i)
        host_in[i] = static_cast<uint64_t>(i) * 3U + 7U;

    // Default stream and an explicit stream must behave identically.
    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);
    REQUIRE(hazeMemcpyAsync(device, host_in.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, stream) ==
            HAZE_SUCCESS);

    std::vector<uint64_t> host_out(kRingDim, 0);
    REQUIRE(hazeMemcpyAsync(host_out.data(), device, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST, nullptr) ==
            HAZE_SUCCESS);
    REQUIRE(host_out == host_in);
}

TEST_CASE("async ops: copy rejects a null destination", "[integration]") {
    configure();
    std::vector<uint64_t> host_in(kRingDim, 1);
    REQUIRE(hazeMemcpyAsync(nullptr, host_in.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: copy rejects a null source", "[integration]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMallocAsync(&device, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(device);
    REQUIRE(hazeMemcpyAsync(device, nullptr, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: copy rejects an unsupported kind", "[integration]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMallocAsync(&device, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(device);
    std::vector<uint64_t> host_in(kRingDim, 5);
    // Value 0 (host-to-host in the CUDA numbering) is outside the supported
    // {1,2,3} kinds and must be rejected.
    const auto unsupported_kind =
        static_cast<hazeMemcpyKind>(0); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    REQUIRE(hazeMemcpyAsync(device, host_in.data(), kBytes, unsupported_kind, nullptr) ==
            HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

// ---------------------------------------------------------------------------
// Async memset ([unit]).
// ---------------------------------------------------------------------------

TEST_CASE("async ops: memset succeeds on the default and an explicit stream", "[unit]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMallocAsync(&device, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(device);
    REQUIRE(hazeMemsetAsync(device, 0, kBytes, nullptr) == HAZE_SUCCESS);

    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);
    REQUIRE(hazeMemsetAsync(device, 0xAB, kBytes, stream) == HAZE_SUCCESS);
}

TEST_CASE("async ops: memset rejects a null pointer", "[unit]") {
    configure();
    REQUIRE(hazeMemsetAsync(nullptr, 0, kBytes, nullptr) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: memset rejects an unknown device pointer", "[unit]") {
    configure();
    REQUIRE(hazeMemsetAsync(unknown_device_ptr(), 0, kBytes, nullptr) ==
            HAZE_ERROR_UNKNOWN_ADDRESS);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: memset rejects a count other than the polynomial size", "[unit]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMallocAsync(&device, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(device);
    REQUIRE(hazeMemsetAsync(device, 0, kBytes + sizeof(uint64_t), nullptr) ==
            HAZE_ERROR_ALLOC_TOO_SMALL);
    (void)hazeGetLastError();
}

TEST_CASE("async ops: memset rejects a freed device pointer", "[unit]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMallocAsync(&device, kBytes, nullptr) == HAZE_SUCCESS);
    DeviceGuard guard(device);
    REQUIRE(hazeFreeAsync(guard.release(), nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMemsetAsync(device, 0, kBytes, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    (void)hazeGetLastError();
}
