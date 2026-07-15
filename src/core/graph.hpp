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
#include "core/epoch.hpp"

#include <expected>
#include <haze/haze_types.h>

namespace haze {

// Backs haze_graph_s: owns the trace snapshot captured at EndCapture.
struct GraphState {
    EpochTraceSnapshot snapshot;
};

// Backs haze_exec_s: a re-dispatchable instance derived from a GraphState,
// holding its own trace snapshot.
struct GraphExec {
    EpochTraceSnapshot snapshot;
};

} // namespace haze

// Opaque-handle struct definitions matching the forward-declared C handles in
// haze_types.h. Allocated via new/delete at the C ABI boundary. Non-copyable;
// the destructor removes the owned on-disk trace copy.
struct haze_graph_s {
    haze::GraphState state;

    haze_graph_s() = default;
    ~haze_graph_s();
    haze_graph_s(const haze_graph_s &) = delete;
    haze_graph_s &operator=(const haze_graph_s &) = delete;
};

struct haze_exec_s {
    haze::GraphExec exec;

    haze_exec_s() = default;
    ~haze_exec_s();
    haze_exec_s(const haze_exec_s &) = delete;
    haze_exec_s &operator=(const haze_exec_s &) = delete;
};

namespace haze {

// Free-function contract for the src/api/graph.cpp shims. Each returns the
// internal std::expected currency; the shims translate to hazeError_t at the
// C ABI boundary. The stream parameter is accepted for CUDA-shape parity and
// carries no ordering in this runtime; a null stream is the default stream.

// hazeStreamBeginCapture: enter epoch capture mode on the (default) stream.
std::expected<void, HazeInternalError> graph_begin_capture(hazeStream_t stream) noexcept;

// hazeStreamEndCapture: snapshot the recorded trace, allocate a graph, return it.
std::expected<hazeGraph_t, HazeInternalError> graph_end_capture(hazeStream_t stream) noexcept;

// hazeGraphInstantiate: build a re-dispatchable exec from a captured graph.
std::expected<hazeGraphExec_t, HazeInternalError> graph_instantiate(hazeGraph_t graph) noexcept;

// hazeGraphLaunch: re-dispatch the exec's snapshot (repeatable).
std::expected<void, HazeInternalError> graph_launch(hazeGraphExec_t exec,
                                                    hazeStream_t stream) noexcept;

// hazeGraphExecUpdate: refresh a same-topology exec from a re-captured graph.
std::expected<void, HazeInternalError> graph_exec_update(hazeGraphExec_t exec,
                                                         hazeGraph_t graph) noexcept;

// hazeGraphExecDestroy: free an exec (and its on-disk copy).
std::expected<void, HazeInternalError> graph_exec_destroy(hazeGraphExec_t exec) noexcept;

// hazeGraphDestroy: free a graph (and its on-disk copy).
std::expected<void, HazeInternalError> graph_destroy(hazeGraph_t graph) noexcept;

// Drop every live graph and exec, removing their on-disk trace copies. Invoked
// from the device-reset path (hazeDeviceReset -> reset_all) so a reset returns
// the runtime to a clean slate rather than leaking graph state and temp dirs
// across resets. The monotonic handle-id counter is intentionally NOT rewound,
// so a handle captured before a reset can never alias a handle minted after it.
void graph_reset() noexcept;

} // namespace haze
