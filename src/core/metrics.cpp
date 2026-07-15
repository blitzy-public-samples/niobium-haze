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

#include <atomic>
#include <cstdint>
#include <haze/haze_types.h>

namespace haze {

Metrics &Metrics::instance() noexcept {
    static Metrics inst;
    return inst;
}

void Metrics::add_op(uint64_t n) noexcept {
    op_count_.fetch_add(n, std::memory_order_relaxed);
}

void Metrics::add_bytes_h2d(uint64_t n) noexcept {
    bytes_h2d_.fetch_add(n, std::memory_order_relaxed);
}

void Metrics::add_bytes_d2h(uint64_t n) noexcept {
    bytes_d2h_.fetch_add(n, std::memory_order_relaxed);
}

void Metrics::add_bytes_d2d(uint64_t n) noexcept {
    bytes_d2d_.fetch_add(n, std::memory_order_relaxed);
}

void Metrics::record_flush(uint64_t ns) noexcept {
    flush_count_.fetch_add(1, std::memory_order_relaxed);
    flush_time_ns_total_.fetch_add(ns, std::memory_order_relaxed);
    flush_time_ns_last_.store(ns, std::memory_order_relaxed);
}

hazePerformanceCounters Metrics::snapshot() const noexcept {
    hazePerformanceCounters c;
    c.op_count = op_count_.load(std::memory_order_relaxed);
    c.bytes_h2d = bytes_h2d_.load(std::memory_order_relaxed);
    c.bytes_d2h = bytes_d2h_.load(std::memory_order_relaxed);
    c.bytes_d2d = bytes_d2d_.load(std::memory_order_relaxed);
    c.flush_count = flush_count_.load(std::memory_order_relaxed);
    c.flush_time_ns_total = flush_time_ns_total_.load(std::memory_order_relaxed);
    c.flush_time_ns_last = flush_time_ns_last_.load(std::memory_order_relaxed);
    return c;
}

void Metrics::reset() noexcept {
    op_count_.store(0, std::memory_order_relaxed);
    bytes_h2d_.store(0, std::memory_order_relaxed);
    bytes_d2h_.store(0, std::memory_order_relaxed);
    bytes_d2d_.store(0, std::memory_order_relaxed);
    flush_count_.store(0, std::memory_order_relaxed);
    flush_time_ns_total_.store(0, std::memory_order_relaxed);
    flush_time_ns_last_.store(0, std::memory_order_relaxed);
}

} // namespace haze
