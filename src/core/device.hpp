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
#pragma once

#include "common/errors.hpp"

#include <expected>
#include <haze/haze_types.h>

namespace haze {

// Device ciphertext-modulus envelope (reported as
// hazeDeviceProp::maxCiphertextModuli). Also the upper bound on an MRP
// group's residue count: a valid group cannot span more residues than the
// device supports moduli, so the C-ABI batch entry points reject a larger
// `count` rather than attempting an unbounded reservation.
inline constexpr int kMaxCiphertextModuli = 64;

// Single-device runtime state. Only one device exists; the only valid
// device index is 0. Free functions instead of a class — a class adds
// nothing over a one-int counter and a couple of getters.

int device_count() noexcept;
int device_active() noexcept;
std::expected<void, HazeInternalError> device_set_active(int device) noexcept;
std::expected<void, HazeInternalError> device_fill_properties(hazeDeviceProp *prop,
                                                              int device) noexcept;
void device_reset() noexcept;

// Simulator peer-access topology. Only simulator-representable behavior is
// modeled; physical multi-chip validation is human follow-up.
//
// The model is a fully-connected topology over the valid device ordinals: a
// device is never its own peer, but any two DISTINCT valid devices are
// mutually accessible and peer access between them can be enabled. On the
// single-device simulator (device_count() == 1) the only in-range ordinal is
// 0, so no distinct peer exists: querying (0,0) reports "not a peer" and every
// enable attempt is rejected. The enabled-distinct-peer path is therefore only
// reachable on real multi-chip hardware (human follow-up).
//
// Concurrency / lock order: the active-device ordinal and the enabled-peer
// matrix are shared process-global state. They are protected by a single
// standalone leaf mutex internal to device.cpp. That mutex is NEVER held while
// acquiring the epoch or allocator mutex: the C-ABI peer shims read or mutate
// this state and release the device lock BEFORE they record anything into the
// epoch, so the device mutex sits entirely outside the epoch -> allocator lock
// order (no nesting is possible). The enabled-peer state is fixed-size and
// mutated without allocation, so no exception can cross the noexcept ABI. All
// peer state is cleared by device_reset().
std::expected<void, HazeInternalError> device_enable_peer_access(int peer,
                                                                 unsigned int flags) noexcept;
std::expected<bool, HazeInternalError> device_can_access_peer(int device, int peer) noexcept;

// Authorization gate for a peer copy (backs hazeMemcpyPeerAsync). Both device
// ordinals must be in range (else InvalidArgument). A same-device copy
// (dst_device == src_device) is a degenerate device-to-device copy that needs
// no peer enablement and is always authorized. A copy between DISTINCT devices
// requires peer access from the destination device to the source device to
// have been enabled via device_enable_peer_access (else InvalidArgument). On
// the single-device simulator only the same-device case is representable.
std::expected<void, HazeInternalError> device_peer_copy_authorized(int dst_device,
                                                                   int src_device) noexcept;

} // namespace haze
