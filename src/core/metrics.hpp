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
#include "common/thread_safety.hpp"

#include <cstdint>
#include <haze/haze_types.h>

namespace haze {

// Process-global performance-counter aggregator feeding the public
// hazeGetPerformanceCounters query.
//
// Singleton via instance(). All counter state is plain uint64_t guarded by a
// single mutex; every mutator, snapshot(), and reset() takes that lock. This
// replaces the previous relaxed-atomic design, whose independent per-field
// loads/stores allowed callers to observe a TORN snapshot (fields drawn from
// different instants) or a partially-applied reset, and whose fetch_add wrapped
// silently on overflow (M3). The defined policy is:
//
//   Consistency: snapshot() and reset() are atomic with respect to all
//     mutators. A snapshot observes all seven counters at one instant (no torn
//     read), and reset() zeroes all seven as one indivisible step (no
//     partially-reset state is ever observable).
//
//   Overflow: every cumulative counter SATURATES at UINT64_MAX rather than
//     wrapping. A saturated counter is a truthful "at least this much" upper
//     bound; silent wraparound to a small value (which would look like a
//     regression or a reset that never happened) cannot occur. Saturation is a
//     terminal state for a cumulative counter until the next reset.
//
//   Reset quiescence: reset() acquires the lock and clears everything as one
//     step, so it is coherent even under concurrent mutation. It is intended to
//     be called at a quiescent point (e.g. hazeDeviceReset); a snapshot racing a
//     reset returns either the fully pre-reset or the fully post-reset values,
//     never a mixture.
//
// Lock order: mutex_ is the INNERMOST leaf lock. It may be acquired while the
// epoch or allocator mutex is held (the metric hooks live on those paths), but
// it is NEVER held while acquiring any other Haze lock, and Metrics never calls
// back into another subsystem — so it participates in no lock cycle. Counter
// updates are coarse-grained (one per high-level op, transfer, or flush), so
// the lock adds negligible overhead.
class Metrics {
  public:
    static Metrics &instance() noexcept;

    // op_count += n, saturating at UINT64_MAX.
    void add_op(uint64_t n = 1) noexcept HAZE_EXCLUDES(mutex_);
    // bytes_h2d += n, saturating.
    void add_bytes_h2d(uint64_t n) noexcept HAZE_EXCLUDES(mutex_);
    // bytes_d2h += n, saturating.
    void add_bytes_d2h(uint64_t n) noexcept HAZE_EXCLUDES(mutex_);
    // bytes_d2d += n, saturating.
    void add_bytes_d2d(uint64_t n) noexcept HAZE_EXCLUDES(mutex_);
    // ++flush_count (saturating); flush_time_ns_total += ns (saturating);
    // flush_time_ns_last = ns.
    void record_flush(uint64_t ns) noexcept HAZE_EXCLUDES(mutex_);

    // Coherent snapshot mapped 1:1 onto hazePerformanceCounters, in field order
    // op_count, bytes_h2d, bytes_d2h, bytes_d2d, flush_count,
    // flush_time_ns_total, flush_time_ns_last. Taken under the lock.
    hazePerformanceCounters snapshot() const noexcept HAZE_EXCLUDES(mutex_);

    // Zero every counter as one indivisible step under the lock.
    void reset() noexcept HAZE_EXCLUDES(mutex_);

    Metrics(const Metrics &) = delete;
    Metrics &operator=(const Metrics &) = delete;

  private:
    Metrics() = default;

    mutable HazeMutex mutex_;
    uint64_t op_count_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t bytes_h2d_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t bytes_d2h_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t bytes_d2d_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t flush_count_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t flush_time_ns_total_ HAZE_GUARDED_BY(mutex_) = 0;
    uint64_t flush_time_ns_last_ HAZE_GUARDED_BY(mutex_) = 0;
};

inline Metrics &metrics() noexcept {
    return Metrics::instance();
}

} // namespace haze
