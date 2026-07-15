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
#include "core/graph.hpp"

#include "common/errors.hpp"
#include "common/thread_safety.hpp"
#include "core/epoch.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <haze/haze_types.h>
#include <new>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

// Monotonic suffix source for unique exec-owned trace directory names.
std::atomic<uint64_t> g_next_graph_dir_id{1};

// Recursively copy an on-disk trace directory into a fresh unique directory
// under the system temp path so the exec owns a copy independent of the source
// graph. Non-throwing: uses the std::error_code filesystem overloads only.
std::expected<std::filesystem::path, haze::HazeInternalError>
clone_trace_dir(const std::filesystem::path &src) noexcept {
    if (src.empty())
        return std::unexpected(haze::HazeInternalError::SourceUnavailable);
    std::error_code ec;
    std::filesystem::path dst =
        std::filesystem::temp_directory_path(ec) /
        ("haze_graph_exec_" +
         std::to_string(g_next_graph_dir_id.fetch_add(1, std::memory_order_relaxed)));
    if (ec)
        return std::unexpected(haze::HazeInternalError::BackendReplayFailed);
    std::filesystem::create_directories(dst, ec);
    if (ec)
        return std::unexpected(haze::HazeInternalError::BackendReplayFailed);
    std::filesystem::copy(src, dst,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(dst, cleanup_ec);
        return std::unexpected(haze::HazeInternalError::BackendReplayFailed);
    }
    return dst;
}

// Live-handle registries. A graph or exec handle is valid only while it is
// registered here: an entry is added when the handle is created and removed
// when it is destroyed. Every entry point validates its incoming handle
// against these sets instead of dereferencing it, so a stale (already
// destroyed), double-freed, or otherwise unknown handle is rejected at the C
// ABI boundary rather than faulting on freed memory. g_handle_mutex is taken
// before the epoch mutex in graph_launch and is never held while the epoch
// mutex is acquired elsewhere, so no lock-order cycle exists.
haze::HazeMutex g_handle_mutex;
std::unordered_set<hazeGraph_t> g_live_graphs HAZE_GUARDED_BY(g_handle_mutex);
std::unordered_set<hazeGraphExec_t> g_live_execs HAZE_GUARDED_BY(g_handle_mutex);

} // namespace

// Handle destructors remove the owned on-disk trace copy, best-effort; the
// error_code overload never throws, matching the implicit noexcept destructor.
haze_graph_s::~haze_graph_s() {
    if (!state.snapshot.project_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(state.snapshot.project_dir, ec);
    }
}

haze_exec_s::~haze_exec_s() {
    if (!exec.snapshot.project_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(exec.snapshot.project_dir, ec);
    }
}

namespace haze {

std::expected<void, HazeInternalError> graph_begin_capture(hazeStream_t /*stream*/) noexcept {
    EpochSession session;                  // backend init + epoch lock
    return epoch().begin_capture_locked(); // sets capturing_; rejects nested capture
}

std::expected<hazeGraph_t, HazeInternalError> graph_end_capture(hazeStream_t /*stream*/) noexcept {
    // Take the snapshot under the epoch lock, then release it before touching
    // the handle registry, so the epoch mutex is never held while g_handle_mutex
    // is acquired (graph_launch takes them in the opposite order).
    EpochTraceSnapshot snapshot;
    {
        HazeLockGuard lock(epoch().mutex());
        auto captured = epoch().end_capture_snapshot_locked();
        if (!captured)
            return std::unexpected(captured.error());
        snapshot = std::move(*captured);
    }
    auto *graph = new (std::nothrow) haze_graph_s{};
    if (graph == nullptr) {
        std::error_code ec;
        std::filesystem::remove_all(snapshot.project_dir, ec); // don't leak the snapshot dir
        return std::unexpected(HazeInternalError::BackendInitFailed);
    }
    graph->state.snapshot = std::move(snapshot);
    {
        HazeLockGuard reg(g_handle_mutex);
        g_live_graphs.insert(graph);
    }
    return graph;
}

std::expected<hazeGraphExec_t, HazeInternalError> graph_instantiate(hazeGraph_t graph) noexcept {
    if (graph == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    if (!g_live_graphs.contains(graph)) // reject a stale or unknown graph handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto cloned = clone_trace_dir(graph->state.snapshot.project_dir);
    if (!cloned)
        return std::unexpected(cloned.error());
    auto *exec = new (std::nothrow) haze_exec_s{};
    if (exec == nullptr) {
        std::error_code ec;
        std::filesystem::remove_all(*cloned, ec);
        return std::unexpected(HazeInternalError::BackendInitFailed);
    }
    // Copy the full snapshot (outputs, target, and the index-parallel
    // output_values that replay_snapshot_locked re-applies each launch), then
    // swap in the exec's own independent on-disk copy.
    exec->exec.snapshot = graph->state.snapshot;
    exec->exec.snapshot.project_dir = std::move(*cloned);
    g_live_execs.insert(exec);
    return exec;
}

std::expected<void, HazeInternalError> graph_launch(hazeGraphExec_t exec,
                                                    hazeStream_t /*stream*/) noexcept {
    if (exec == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    if (!g_live_execs.contains(exec)) // reject a stale or unknown exec handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(epoch().mutex());
    return epoch().replay_snapshot_locked(exec->exec.snapshot);
}

std::expected<void, HazeInternalError> graph_exec_update(hazeGraphExec_t exec,
                                                         hazeGraph_t graph) noexcept {
    if (exec == nullptr || graph == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    if (!g_live_execs.contains(exec) || !g_live_graphs.contains(graph))
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Same-topology proxy: the two snapshots must bind the same ordered set of
    // output addresses. Equal counts alone would accept a different operation
    // that writes the same number of outputs (e.g. a multiply refreshing an
    // add-exec); a valid refresh only rebinds inputs and leaves the tagged
    // output addresses unchanged, so a differing output address is a topology
    // mismatch and is rejected.
    const auto &exec_outputs = exec->exec.snapshot.outputs;
    const auto &graph_outputs = graph->state.snapshot.outputs;
    if (exec_outputs.size() != graph_outputs.size())
        return std::unexpected(HazeInternalError::InvalidArgument);
    for (std::size_t i = 0; i < exec_outputs.size(); ++i)
        if (exec_outputs[i].first != graph_outputs[i].first)
            return std::unexpected(HazeInternalError::InvalidArgument);
    auto cloned = clone_trace_dir(graph->state.snapshot.project_dir);
    if (!cloned)
        return std::unexpected(cloned.error());
    std::error_code ec;
    std::filesystem::remove_all(exec->exec.snapshot.project_dir, ec); // drop the stale copy
    exec->exec.snapshot = graph->state.snapshot;
    exec->exec.snapshot.project_dir = std::move(*cloned);
    return {};
}

std::expected<void, HazeInternalError> graph_exec_destroy(hazeGraphExec_t exec) noexcept {
    if (exec == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    auto it = g_live_execs.find(exec);
    if (it == g_live_execs.end()) // reject a stale or double-freed exec handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    g_live_execs.erase(it);
    delete exec;
    return {};
}

std::expected<void, HazeInternalError> graph_destroy(hazeGraph_t graph) noexcept {
    if (graph == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    auto it = g_live_graphs.find(graph);
    if (it == g_live_graphs.end()) // reject a stale or double-freed graph handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    g_live_graphs.erase(it);
    delete graph;
    return {};
}

} // namespace haze
