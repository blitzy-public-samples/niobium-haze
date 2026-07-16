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
#include "core/stream.hpp"

#include "common/thread_safety.hpp"

#include <atomic>
#include <cstdint>
#include <haze/haze_types.h>
#include <new>
#include <unordered_set>

namespace haze {

namespace {
std::atomic<uint64_t> g_next_stream_id{1};
std::atomic<uint64_t> g_next_event_id{1};

// Live-handle registries: each set holds exactly the handles currently owned
// by a caller (created and not yet destroyed). create() inserts, destroy()
// removes and frees, so the registry never holds a dangling pointer and a
// foreign or double destroy is detected as a membership miss. Guarded by a
// dedicated mutex; this subsystem never calls the epoch or allocator, so it is
// outside the epoch -> allocator lock order.
HazeMutex g_handle_mutex;
std::unordered_set<hazeStream_t> g_live_streams HAZE_GUARDED_BY(g_handle_mutex);
std::unordered_set<hazeEvent_t> g_live_events HAZE_GUARDED_BY(g_handle_mutex);
} // namespace

hazeStream_t stream_create() noexcept {
    auto *s = new (std::nothrow)
        haze_stream_s{.id = g_next_stream_id.fetch_add(1, std::memory_order_relaxed)};
    if (s == nullptr) {
        return nullptr;
    }
    // Registering allocates a set node, which can throw under memory pressure;
    // keep the noexcept boundary intact by treating that as an allocation
    // failure (free the handle and report OOM to the caller).
    try {
        HazeLockGuard lock(g_handle_mutex);
        g_live_streams.insert(s);
    } catch (...) {
        delete s;
        return nullptr;
    }
    return s;
}

bool stream_destroy(hazeStream_t s) noexcept {
    // Destroying NULL is a well-defined no-op success (mirrors delete nullptr
    // and the documented hazeStreamDestroy(NULL) behavior).
    if (s == nullptr) {
        return true;
    }
    {
        HazeLockGuard lock(g_handle_mutex);
        // A foreign or already-destroyed handle is not present: reject it
        // without deleting so the caller returns HAZE_ERROR_INVALID_VALUE
        // rather than aborting on an invalid/double delete.
        if (g_live_streams.erase(s) == 0) {
            return false;
        }
    }
    delete s;
    return true;
}

void streams_reset() noexcept {
    g_next_stream_id.store(1, std::memory_order_relaxed);
}

hazeEvent_t event_create() noexcept {
    auto *e = new (std::nothrow)
        haze_event_s{.id = g_next_event_id.fetch_add(1, std::memory_order_relaxed)};
    if (e == nullptr) {
        return nullptr;
    }
    try {
        HazeLockGuard lock(g_handle_mutex);
        g_live_events.insert(e);
    } catch (...) {
        delete e;
        return nullptr;
    }
    return e;
}

bool event_destroy(hazeEvent_t e) noexcept {
    if (e == nullptr) {
        return true;
    }
    {
        HazeLockGuard lock(g_handle_mutex);
        if (g_live_events.erase(e) == 0) {
            return false;
        }
    }
    delete e;
    return true;
}

void event_record(hazeEvent_t /*e*/) noexcept {
    // Events do not model ordering in this runtime (CUDA-shape parity
    // only); recording is a no-op.
}

void events_reset() noexcept {
    g_next_event_id.store(1, std::memory_order_relaxed);
}

} // namespace haze
