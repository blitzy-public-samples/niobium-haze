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

#include <atomic>
#include <cstdint>
#include <haze/haze_types.h>

namespace haze {

// Process-global performance-counter aggregator; lock-free atomics feed the
// public hazeGetPerformanceCounters query.
//
// Singleton via instance(). Counters are std::atomic<uint64_t>; mutators and
// reset() use relaxed stores/fetch-adds, snapshot() uses relaxed loads.
class Metrics {
  public:
    static Metrics &instance() noexcept;

    // op_count_ += n.
    void add_op(uint64_t n = 1) noexcept;
    // bytes_h2d_ += n.
    void add_bytes_h2d(uint64_t n) noexcept;
    // bytes_d2h_ += n.
    void add_bytes_d2h(uint64_t n) noexcept;
    // bytes_d2d_ += n.
    void add_bytes_d2d(uint64_t n) noexcept;
    // ++flush_count_; flush_time_ns_total_ += ns; flush_time_ns_last_ = ns.
    void record_flush(uint64_t ns) noexcept;

    // Relaxed-load snapshot mapped 1:1 onto hazePerformanceCounters, in field
    // order op_count, bytes_h2d, bytes_d2h, bytes_d2d, flush_count,
    // flush_time_ns_total, flush_time_ns_last.
    hazePerformanceCounters snapshot() const noexcept;

    // Relaxed-store zero into every counter.
    void reset() noexcept;

    Metrics(const Metrics &) = delete;
    Metrics &operator=(const Metrics &) = delete;

  private:
    Metrics() = default;

    std::atomic<uint64_t> op_count_{0};
    std::atomic<uint64_t> bytes_h2d_{0};
    std::atomic<uint64_t> bytes_d2h_{0};
    std::atomic<uint64_t> bytes_d2d_{0};
    std::atomic<uint64_t> flush_count_{0};
    std::atomic<uint64_t> flush_time_ns_total_{0};
    std::atomic<uint64_t> flush_time_ns_last_{0};
};

inline Metrics &metrics() noexcept {
    return Metrics::instance();
}

} // namespace haze
