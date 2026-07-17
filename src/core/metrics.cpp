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
#include "core/metrics.hpp"

#include "common/thread_safety.hpp"

#include <cstdint>
#include <haze/haze_types.h>
#include <limits>

namespace haze {

namespace {
// Add `n` to `counter` in place, clamping at UINT64_MAX instead of wrapping.
// Called only with mutex_ held (all callers below take the lock first).
void add_saturating(uint64_t &counter, uint64_t n) noexcept {
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    if (counter > kMax - n) {
        counter = kMax;
    } else {
        counter += n;
    }
}
} // namespace

Metrics &Metrics::instance() noexcept {
    static Metrics inst;
    return inst;
}

void Metrics::add_op(uint64_t n) noexcept {
    HazeLockGuard lock(mutex_);
    add_saturating(op_count_, n);
}

void Metrics::add_bytes_h2d(uint64_t n) noexcept {
    HazeLockGuard lock(mutex_);
    add_saturating(bytes_h2d_, n);
}

void Metrics::add_bytes_d2h(uint64_t n) noexcept {
    HazeLockGuard lock(mutex_);
    add_saturating(bytes_d2h_, n);
}

void Metrics::add_bytes_d2d(uint64_t n) noexcept {
    HazeLockGuard lock(mutex_);
    add_saturating(bytes_d2d_, n);
}

void Metrics::record_flush(uint64_t ns) noexcept {
    HazeLockGuard lock(mutex_);
    add_saturating(flush_count_, 1);
    add_saturating(flush_time_ns_total_, ns);
    // Most-recent flush wall time: last-writer-wins. A single store under the
    // lock, so it is never torn; concurrent flushes leave one of their times.
    flush_time_ns_last_ = ns;
}

hazePerformanceCounters Metrics::snapshot() const noexcept {
    // One lock acquisition copies all seven counters, so the returned struct is
    // internally consistent (every field is from the same instant).
    HazeLockGuard lock(mutex_);
    hazePerformanceCounters c;
    c.op_count = op_count_;
    c.bytes_h2d = bytes_h2d_;
    c.bytes_d2h = bytes_d2h_;
    c.bytes_d2d = bytes_d2d_;
    c.flush_count = flush_count_;
    c.flush_time_ns_total = flush_time_ns_total_;
    c.flush_time_ns_last = flush_time_ns_last_;
    return c;
}

void Metrics::reset() noexcept {
    // Zero all seven under one lock: a concurrent snapshot sees either the full
    // pre-reset or full post-reset state, never a partial reset.
    HazeLockGuard lock(mutex_);
    op_count_ = 0;
    bytes_h2d_ = 0;
    bytes_d2h_ = 0;
    bytes_d2d_ = 0;
    flush_count_ = 0;
    flush_time_ns_total_ = 0;
    flush_time_ns_last_ = 0;
}

} // namespace haze
