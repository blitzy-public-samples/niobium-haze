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
#include "core/device.hpp"

#include "common/errors.hpp"
#include "common/thread_safety.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <haze/haze_types.h>

namespace haze {

namespace {
inline constexpr int kDeviceCount = 1;
inline constexpr size_t kHbmSize = 16ULL * 1024 * 1024 * 1024; // 16 GB
inline constexpr int kNumRegisters = 64;
inline constexpr int kNumHbmBanks = 8;
// Ring dimension exponents 10..16 → N = 1024..65536.
inline constexpr int kSupportedRingDimExponents[] = {10, 11, 12, 13, 14, 15, 16};
inline constexpr int kNumSupportedRingDims =
    static_cast<int>(sizeof(kSupportedRingDimExponents) / sizeof(kSupportedRingDimExponents[0]));

// Standalone leaf mutex guarding all mutable device/peer state below. It is
// NEVER held while acquiring the epoch or allocator mutex (see the lock-order
// note in device.hpp), so it participates in no lock cycle. Guarding this
// state fixes the P2 data race on g_active_device and the enabled-peer set.
HazeMutex g_device_mutex;

// The active device ordinal. Only 0 is ever valid, so in this single-device
// build it never leaves 0; it is still guarded so a concurrent
// hazeSetDevice / hazeGetDevice pair is race-free.
int g_active_device HAZE_GUARDED_BY(g_device_mutex) = 0;

// Enabled peer-access matrix: g_peer_enabled[from][to] == true means the
// "from" device has enabled access to the "to" device's memory. This replaces
// the old std::set<std::pair<int,int>> whose insert() allocates and could throw
// std::bad_alloc through the noexcept ABI (P2). A fixed-size std::array is
// trivially copyable and mutated without allocation, so no exception can cross
// the boundary. On the single-device simulator no distinct peer is ever
// enabled, so every cell stays false; the matrix still models the authorized
// distinct-peer path that real multi-chip hardware would populate.
using PeerMatrix = std::array<std::array<bool, kDeviceCount>, kDeviceCount>;
PeerMatrix g_peer_enabled HAZE_GUARDED_BY(g_device_mutex) = {};

// A device index is valid when it lies within the current topology. Reads only
// the compile-time device count, so it needs no lock.
bool valid_device(int d) noexcept {
    return d >= 0 && d < kDeviceCount;
}
} // namespace

int device_count() noexcept {
    return kDeviceCount;
}

int device_active() noexcept {
    HazeLockGuard lock(g_device_mutex);
    return g_active_device;
}

std::expected<void, HazeInternalError> device_set_active(int device) noexcept {
    if (device != 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(g_device_mutex);
    g_active_device = device;
    return {};
}

std::expected<void, HazeInternalError> device_fill_properties(hazeDeviceProp *prop,
                                                              int device) noexcept {
    if (prop == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    if (device != 0)
        return std::unexpected(HazeInternalError::InvalidArgument);

    *prop = {};
    std::strncpy(prop->name, "Niobium FPGA", sizeof(prop->name) - 1);
    prop->name[sizeof(prop->name) - 1] = '\0';
    prop->totalGlobalMem = kHbmSize;
    prop->numRegisters = kNumRegisters;
    prop->numSupportedRingDims = kNumSupportedRingDims;
    for (int i = 0; i < kNumSupportedRingDims; i++) {
        prop->supportedRingDimExponents[i] = kSupportedRingDimExponents[i];
    }
    prop->maxCiphertextModuli = kMaxCiphertextModuli;
    prop->numHBMBanks = kNumHbmBanks;
    return {};
}

void device_reset() noexcept {
    HazeLockGuard lock(g_device_mutex);
    g_active_device = 0;
    g_peer_enabled = {};
}

std::expected<void, HazeInternalError> device_enable_peer_access(int peer,
                                                                 unsigned int flags) noexcept {
    // Validate the arguments (which read no mutable state) before taking the
    // lock. CUDA rejects a non-zero flags value; we mirror that.
    if (flags != 0)
        return std::unexpected(HazeInternalError::InvalidArgument);
    if (!valid_device(peer))
        return std::unexpected(HazeInternalError::InvalidArgument);

    HazeLockGuard lock(g_device_mutex);
    // A device cannot enable itself as a peer (matches cudaErrorInvalidDevice
    // when peerDevice == the current device). Read the active ordinal directly
    // under the held lock rather than via device_active(), which would try to
    // re-acquire this same mutex. On the single-device simulator the only valid
    // ordinal equals the active device, so this branch always rejects — there
    // is genuinely no distinct peer to authorize. The assignment below is the
    // multi-chip-hardware path (human follow-up).
    if (peer == g_active_device)
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Both indices are non-negative and < kDeviceCount (valid_device above and
    // the active ordinal invariant), so the cast to the array's size_type is
    // value-preserving.
    g_peer_enabled[static_cast<std::size_t>(g_active_device)][static_cast<std::size_t>(peer)] =
        true;
    return {};
}

std::expected<bool, HazeInternalError> device_can_access_peer(int device, int peer) noexcept {
    if (!valid_device(device) || !valid_device(peer))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Capability is a static property of the topology, independent of the
    // enable state, so no lock is needed. A device is never its own peer
    // (matches cudaDeviceCanAccessPeer); any two DISTINCT valid devices are
    // mutually accessible. On the single-device simulator the only in-range
    // query is (0,0), which reports false.
    return device != peer;
}

std::expected<void, HazeInternalError> device_peer_copy_authorized(int dst_device,
                                                                   int src_device) noexcept {
    if (!valid_device(dst_device) || !valid_device(src_device))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // A same-device copy is a degenerate device-to-device copy: it needs no
    // peer authorization (the destination is reading its own memory space).
    if (dst_device == src_device)
        return {};
    // A copy between DISTINCT devices is a true peer copy: it is permitted only
    // if the destination device has enabled peer access to the source device.
    // Unreachable on the single-device simulator (distinct valid ordinals do
    // not exist there); live on multi-chip hardware.
    HazeLockGuard lock(g_device_mutex);
    // Both ordinals are validated non-negative and < kDeviceCount above, so the
    // cast to the array's size_type is value-preserving.
    if (!g_peer_enabled[static_cast<std::size_t>(dst_device)][static_cast<std::size_t>(src_device)])
        return std::unexpected(HazeInternalError::InvalidArgument);
    return {};
}

} // namespace haze
