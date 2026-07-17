// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Introspection surface of the public C ABI: what haze reports about a pointer
// through hazePointerGetAttributes. These cases assert the full attribute
// record — memory type, the matching device/host pointer field, and the device
// ordinal — for device, host, null, and freed pointers, and confirm that
// pointers handed out by an MRP residue-group allocation introspect as device
// memory.
//
// Coverage is deliberately non-overlapping with the baseline suites:
//   - test_memory.cpp already asserts the .type field for device / host /
//     foreign pointers and owns the full hazeMallocMrp / hazeFreeMrp validation
//     matrix (count 0, wrong size, out-of-range count, null array, double
//     free, skipped-null entries). Those are not repeated here; this file adds
//     the field-level and null / post-free classification that test_memory
//     does not exercise, plus the MRP-to-introspection tie-in.
//   - test_error_semantics.cpp owns hazeGetErrorString, so no error-string
//     cases live here.
// Every case is a pure allocator / introspection operation and is tagged
// [unit].

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Frees a device pointer on scope exit unless released.
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

// Frees a host allocation on scope exit.
class HostGuard {
  public:
    explicit HostGuard(void *p) noexcept : p_(p) {}
    HostGuard(const HostGuard &) = delete;
    HostGuard &operator=(const HostGuard &) = delete;
    HostGuard(HostGuard &&) = delete;
    HostGuard &operator=(HostGuard &&) = delete;
    ~HostGuard() {
        if (p_ != nullptr)
            (void)hazeFreeHost(p_);
    }
    void *get() const noexcept { return p_; }

  private:
    void *p_ = nullptr;
};

// Frees an MRP residue group on scope exit.
class MrpGuard {
  public:
    explicit MrpGuard(std::vector<void *> ptrs) noexcept : ptrs_(std::move(ptrs)) {}
    MrpGuard(const MrpGuard &) = delete;
    MrpGuard &operator=(const MrpGuard &) = delete;
    MrpGuard(MrpGuard &&) = delete;
    MrpGuard &operator=(MrpGuard &&) = delete;
    ~MrpGuard() {
        if (!ptrs_.empty())
            (void)hazeFreeMrp(ptrs_.data(), ptrs_.size());
    }
    std::vector<void *> &ptrs() noexcept { return ptrs_; }

  private:
    std::vector<void *> ptrs_;
};

void configure() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

} // namespace

// ---------------------------------------------------------------------------
// Full pointer-attribute record for each memory class.
// ---------------------------------------------------------------------------

TEST_CASE("introspection: a device pointer reports device type and its own address", "[unit]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMalloc(&device, kBytes) == HAZE_SUCCESS);
    REQUIRE(device != nullptr);
    DeviceGuard guard(device);

    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, device) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_DEVICE);
    REQUIRE(attr.devicePointer == device);
    REQUIRE(attr.hostPointer == nullptr);
    REQUIRE(attr.device == 0); // single-device simulator
}

TEST_CASE("introspection: a host pointer reports host type and its own address", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    void *host = nullptr;
    REQUIRE(hazeHostAlloc(&host, kBytes, 0U) == HAZE_SUCCESS);
    REQUIRE(host != nullptr);
    HostGuard guard(host);

    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, host) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_HOST);
    REQUIRE(attr.hostPointer == host);
    REQUIRE(attr.devicePointer == nullptr);
    REQUIRE(attr.device == 0);
}

TEST_CASE("introspection: a null pointer classifies as unregistered", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, nullptr) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_UNREGISTERED);
    REQUIRE(attr.devicePointer == nullptr);
    REQUIRE(attr.hostPointer == nullptr);
    REQUIRE(attr.device == 0);
}

TEST_CASE("introspection: a freed device pointer reclassifies as unregistered", "[unit]") {
    configure();
    void *device = nullptr;
    REQUIRE(hazeMalloc(&device, kBytes) == HAZE_SUCCESS);
    REQUIRE(device != nullptr);
    DeviceGuard guard(device);
    REQUIRE(hazeFree(guard.release()) == HAZE_SUCCESS);

    // The pointer VALUE is inspected, never dereferenced: liveness has been
    // dropped, so the classification falls back to unregistered.
    hazePointerAttributes attr{};
    REQUIRE(hazePointerGetAttributes(&attr, device) == HAZE_SUCCESS);
    REQUIRE(attr.type == HAZE_MEMORY_TYPE_UNREGISTERED);
    REQUIRE(attr.devicePointer == nullptr);
    REQUIRE(attr.hostPointer == nullptr);
}

// ---------------------------------------------------------------------------
// MRP residue group introspects as device memory.
// ---------------------------------------------------------------------------

TEST_CASE("introspection: MRP residue pointers each classify as device memory", "[unit]") {
    configure();
    constexpr std::size_t kCount = 3;
    MrpGuard group(std::vector<void *>(kCount, nullptr));
    REQUIRE(hazeMallocMrp(group.ptrs().data(), kCount, kBytes) == HAZE_SUCCESS);

    for (std::size_t i = 0; i < kCount; ++i) {
        REQUIRE(group.ptrs()[i] != nullptr);
        hazePointerAttributes attr{};
        REQUIRE(hazePointerGetAttributes(&attr, group.ptrs()[i]) == HAZE_SUCCESS);
        REQUIRE(attr.type == HAZE_MEMORY_TYPE_DEVICE);
        REQUIRE(attr.devicePointer == group.ptrs()[i]);
        REQUIRE(attr.device == 0);
        // Residues within a group are distinct addresses.
        for (std::size_t j = i + 1; j < kCount; ++j)
            REQUIRE(group.ptrs()[i] != group.ptrs()[j]);
    }
}
