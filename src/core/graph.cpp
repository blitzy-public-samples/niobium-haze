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
#include "common/log.hpp"
#include "common/thread_safety.hpp"
#include "core/epoch.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <haze/haze_types.h>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

// Token-keyed live-handle registries (G6).
//
// A graph/exec handle handed to the caller is NOT a heap pointer to the owning
// object — it is an opaque monotonic token value reinterpret_cast to the
// opaque C handle type. The real objects live in these maps, owned by
// unique_ptr and keyed by that token. Every entry point converts its incoming
// handle back to a token and LOOKS IT UP; the user's handle bits are never
// dereferenced. This closes the ABA / use-after-free window the previous
// raw-address std::unordered_set registries left open: there, a graph freed
// (its heap block returned to the allocator) and a *different* new graph handed
// the SAME address would wrongly validate against the set. Tokens are never
// reused (see g_next_handle_id), so a stale token can never collide with a live
// one.
//
// Lock order: g_handle_mutex is acquired before the epoch mutex in
// graph_launch and is never held while the epoch mutex is taken elsewhere, so
// no lock-order cycle exists.
haze::HazeMutex g_handle_mutex;
std::unordered_map<uint64_t, std::unique_ptr<haze_graph_s>>
    g_graphs HAZE_GUARDED_BY(g_handle_mutex);
std::unordered_map<uint64_t, std::unique_ptr<haze_exec_s>> g_execs HAZE_GUARDED_BY(g_handle_mutex);
// Strictly-monotonic token source. Token 0 is reserved as "invalid" (never
// issued), so a null handle can never alias a live token. Never rewound — not
// even by graph_reset — so a token minted before a reset can never match one
// minted after it. Guarded by g_handle_mutex.
uint64_t g_next_handle_id HAZE_GUARDED_BY(g_handle_mutex) = 1;

// Opaque-token <-> handle conversions. The token is carried in the pointer's
// bit pattern; the handle is never dereferenced as a pointer.
hazeGraph_t graph_token_to_handle(uint64_t id) noexcept {
    // The opaque handle carries the token in its bit pattern; it is never
    // dereferenced. The optimizer-aliasing performance hint does not apply to
    // handle types (same pattern as haze::to_void_ptr in common/handle.hpp).
    return reinterpret_cast<hazeGraph_t>( // NOLINT(performance-no-int-to-ptr)
        static_cast<uintptr_t>(id));
}
hazeGraphExec_t exec_token_to_handle(uint64_t id) noexcept {
    return reinterpret_cast<hazeGraphExec_t>( // NOLINT(performance-no-int-to-ptr)
        static_cast<uintptr_t>(id));
}
uint64_t graph_handle_to_token(hazeGraph_t h) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
}
uint64_t exec_handle_to_token(hazeGraphExec_t h) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
}

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
    // G5: object allocation, the snapshot move, and map insertion can all throw
    // (bad_alloc / rehash); nothing may escape this noexcept boundary. On any
    // failure the graph-owned on-disk copy must not leak.
    try {
        auto graph = std::make_unique<haze_graph_s>();
        graph->state.snapshot = std::move(snapshot);
        HazeLockGuard reg(g_handle_mutex);
        const uint64_t id = g_next_handle_id++;
        g_graphs.emplace(id, std::move(graph));
        return graph_token_to_handle(id);
    } catch (...) {
        // If make_unique threw, `snapshot` still owns the dir and is removed
        // here. If a later step threw, the unique_ptr already unwound and ran
        // haze_graph_s::~haze_graph_s (which removed the dir); `snapshot` is
        // then moved-from/empty and this remove_all is a harmless no-op.
        std::error_code ec;
        std::filesystem::remove_all(snapshot.project_dir, ec);
        return std::unexpected(HazeInternalError::BackendInitFailed);
    }
}

std::expected<hazeGraphExec_t, HazeInternalError> graph_instantiate(hazeGraph_t graph) noexcept {
    if (graph == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    auto git = g_graphs.find(graph_handle_to_token(graph)); // token lookup; never deref the handle
    if (git == g_graphs.end())                              // stale / unknown / forged handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    // G4: exec gets its OWN private, unpredictable, owner-only copy of the
    // frozen project so it is independent of the source graph's lifetime.
    auto cloned = secure_clone_project_dir(git->second->state.snapshot.project_dir);
    if (!cloned)
        return std::unexpected(cloned.error());
    std::filesystem::path new_dir = std::move(*cloned);
    // G5: the snapshot copy and map insertion can throw; reclaim the freshly
    // cloned dir if any step after the secure clone fails.
    try {
        auto exec = std::make_unique<haze_exec_s>();
        // Copy the graph's snapshot (outputs, generations, target, inputs, sig)...
        exec->exec.snapshot = git->second->state.snapshot;
        // ...then give the exec its own independent on-disk copy.
        exec->exec.snapshot.project_dir = std::move(new_dir);
        const uint64_t id = g_next_handle_id++;
        g_execs.emplace(id, std::move(exec));
        return exec_token_to_handle(id);
    } catch (...) {
        // If the throw preceded the move, new_dir still holds the clone and it
        // is removed here. If it followed, the exec unique_ptr unwound and its
        // destructor removed the dir; new_dir is empty and this is a no-op.
        std::error_code ec;
        std::filesystem::remove_all(new_dir, ec);
        return std::unexpected(HazeInternalError::BackendInitFailed);
    }
}

std::expected<void, HazeInternalError> graph_launch(hazeGraphExec_t exec,
                                                    hazeStream_t /*stream*/) noexcept {
    if (exec == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Hold g_handle_mutex across the whole launch so the exec (and its on-disk
    // project) cannot be destroyed mid-replay by a concurrent graph_exec_destroy
    // / graph_reset. Then take the epoch mutex (handle -> epoch order).
    HazeLockGuard reg(g_handle_mutex);
    auto eit = g_execs.find(exec_handle_to_token(exec)); // token lookup; never deref the handle
    if (eit == g_execs.end())                            // stale / unknown / forged handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard lock(epoch().mutex());
    // Open a fresh correlation context and trace span for this launch so every
    // diagnostic emitted during the replay -- including replay_bridge lines on
    // this thread during the crossing -- shares one id (O1 propagation).
    const CorrelationScope cid_scope(next_correlation_id());
    TraceSpan span("graph.launch");
    // replay_snapshot_locked is itself noexcept (wraps its own throwing replay
    // and JSON readback), so no exception escapes here.
    auto result = epoch().replay_snapshot_locked(eit->second->exec.snapshot);
    if (!result)
        span.mark_error();
    return result;
}

std::expected<void, HazeInternalError> graph_exec_update(hazeGraphExec_t exec,
                                                         hazeGraph_t graph) noexcept {
    if (exec == nullptr || graph == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    auto eit = g_execs.find(exec_handle_to_token(exec));
    auto git = g_graphs.find(graph_handle_to_token(graph));
    if (eit == g_execs.end() || git == g_graphs.end())
        return std::unexpected(HazeInternalError::InvalidArgument);
    auto &exec_snap = eit->second->exec.snapshot;
    const auto &graph_snap = git->second->state.snapshot;

    // G3 same-topology check. A valid refresh rebinds inputs but preserves both
    // the tagged output addresses AND the recorded operation sequence.
    //
    // 1) Output-address check: the two snapshots must bind the same ordered set
    //    of output addresses. This rejects a differing output cardinality or a
    //    graph that tags a different device address than the exec it refreshes.
    if (exec_snap.outputs.size() != graph_snap.outputs.size())
        return std::unexpected(HazeInternalError::InvalidArgument);
    for (std::size_t i = 0; i < exec_snap.outputs.size(); ++i)
        if (exec_snap.outputs[i].first != graph_snap.outputs[i].first)
            return std::unexpected(HazeInternalError::InvalidArgument);
    // 2) Operation-topology check: the recorded op-sequences must be
    //    structurally identical. The output-address check alone would accept a
    //    different operation that happens to write the SAME output address
    //    (e.g. a multiply refreshing an add-exec at the same destination),
    //    silently changing what the exec computes. The trace signature hashes
    //    the instruction stream with operand register numbers canonicalized by
    //    first-appearance order, so it is invariant to a pure input rebind yet
    //    still distinguishes a genuine operation change. A zero signature means
    //    it could not be computed for one side; treat that as a mismatch rather
    //    than accepting an unverifiable refresh.
    const uint64_t exec_sig = exec_snap.trace_signature;
    const uint64_t graph_sig = graph_snap.trace_signature;
    if (exec_sig == 0 || graph_sig == 0 || exec_sig != graph_sig)
        return std::unexpected(HazeInternalError::InvalidArgument);

    // G4: build the replacement copy securely.
    auto cloned = secure_clone_project_dir(graph_snap.project_dir);
    if (!cloned)
        return std::unexpected(cloned.error());

    // G7: build the ENTIRE replacement snapshot as a local FIRST, so the only
    // throwing work (the deep copy of the graph snapshot) happens with the exec
    // still fully intact. If the copy throws, the exec is untouched and the
    // freshly cloned dir is reclaimed — no half-updated, unlaunchable exec.
    EpochTraceSnapshot replacement;
    try {
        replacement = graph_snap; // deep copy: outputs / generations / target / inputs / sig
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(*cloned, ec);
        return std::unexpected(HazeInternalError::BackendInitFailed);
    }
    replacement.project_dir = std::move(*cloned);

    // Commit via a noexcept swap: after this the exec is bound to the NEW dir,
    // and `replacement` holds the OLD snapshot (including the old dir).
    std::swap(exec_snap, replacement);

    // Only now tear down the superseded on-disk copy. Because the swap already
    // installed the new dir, a failure here still leaves a valid, launchable
    // exec (G7). The failure is observable rather than silently swallowed.
    if (!replacement.project_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(replacement.project_dir, ec);
        if (ec)
            log_error("graph_exec_update",
                      "failed to remove superseded trace directory after update");
    }
    return {};
}

std::expected<void, HazeInternalError> graph_exec_destroy(hazeGraphExec_t exec) noexcept {
    if (exec == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    auto it = g_execs.find(exec_handle_to_token(exec));
    if (it == g_execs.end()) // reject a stale or double-freed exec handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Erasing runs haze_exec_s::~haze_exec_s, which removes the on-disk copy.
    // erase() on an unordered_map node is noexcept.
    g_execs.erase(it);
    return {};
}

std::expected<void, HazeInternalError> graph_destroy(hazeGraph_t graph) noexcept {
    if (graph == nullptr)
        return std::unexpected(HazeInternalError::InvalidArgument);
    HazeLockGuard reg(g_handle_mutex);
    auto it = g_graphs.find(graph_handle_to_token(graph));
    if (it == g_graphs.end()) // reject a stale or double-freed graph handle
        return std::unexpected(HazeInternalError::InvalidArgument);
    // Erasing runs haze_graph_s::~haze_graph_s, which removes the on-disk copy.
    g_graphs.erase(it);
    return {};
}

void graph_reset() noexcept {
    HazeLockGuard reg(g_handle_mutex);
    // Destroy execs before graphs (execs own independent clones; ordering is not
    // strictly required since each owns its own dir, but execs are the derived
    // objects). Each erased unique_ptr runs its handle destructor, removing the
    // on-disk trace copy. g_next_handle_id is deliberately left untouched so
    // post-reset tokens never alias pre-reset ones.
    g_execs.clear();
    g_graphs.clear();
}

} // namespace haze
