// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Tests for hazeGetPerformanceCounters (P2b). It writes a hazePerformanceCounters
// snapshot of cumulative runtime counters: op_count and the flush counters
// advance through the record -> hazeFlush -> replay path, while the byte
// counters (H2D / D2H / D2D) advance synchronously at each hazeMemcpy.
// hazeDeviceReset zeroes every counter. Flush timings are wall-clock
// nanoseconds and may legitimately read zero on fast hardware.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);

// Read the current counter snapshot through the public query. Aborts the
// enclosing Catch2 assertion scope if the query itself fails.
hazePerformanceCounters read_counters() {
    hazePerformanceCounters pc{};
    REQUIRE(hazeGetPerformanceCounters(&pc) == HAZE_SUCCESS);
    return pc;
}

// Record a single SRP add, tag its output, flush (a genuine replay), then read
// the result back to the host. Returns the counter snapshot taken after the
// D2H so callers can assert the op / flush / byte counters from one shared
// workload. Two host-to-device inputs contribute 2 * kBytes to bytes_h2d and
// the readback contributes kBytes to bytes_d2h.
hazePerformanceCounters run_add_flush_workload() {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);
    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(q, /*seed=*/1, kRingDim),
        haze::test::make_residue(q, /*seed=*/2, kRingDim),
    };
    auto d_in = haze::test::allocate_and_h2d_residues(inputs);
    auto d_dst = haze::test::allocate_dst_residues(/*count=*/1, kBytes);

    REQUIRE(hazeAdd(d_dst[0], d_in[0], d_in[1], /*mod_idx=*/0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst[0]) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), d_dst[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    const hazePerformanceCounters pc = read_counters();
    haze::test::free_all_residues(d_in);
    haze::test::free_all_residues(d_dst);
    return pc;
}

} // namespace

// ---------------------------------------------------------------------------
// Phase 1 — argument validation, struct layout, and reset semantics ([unit]).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: hazeGetPerformanceCounters rejects a null pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetPerformanceCounters(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("perf counters: the counters struct has the documented size", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    STATIC_REQUIRE(sizeof(hazePerformanceCounters) == 7 * sizeof(uint64_t));
}

TEST_CASE("perf counters: a fresh device reset zeroes all counters", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazePerformanceCounters pc = read_counters();
    REQUIRE(pc.op_count == 0);
    REQUIRE(pc.bytes_h2d == 0);
    REQUIRE(pc.bytes_d2h == 0);
    REQUIRE(pc.bytes_d2d == 0);
    REQUIRE(pc.flush_count == 0);
    REQUIRE(pc.flush_time_ns_total == 0);
    REQUIRE(pc.flush_time_ns_last == 0);
}

// ---------------------------------------------------------------------------
// Phase 2 — synchronous byte counters ([integration]; no flush required).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: a host-to-device copy increments the H2D byte counter", "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> host(kRingDim);
    REQUIRE(hazeMemcpy(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const hazePerformanceCounters pc = read_counters();
    REQUIRE(pc.bytes_h2d >= kBytes);

    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
}

TEST_CASE("perf counters: counters accumulate across multiple host-to-device copies",
          "[integration]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> host(kRingDim);
    REQUIRE(hazeMemcpy(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    const hazePerformanceCounters pc = read_counters();
    REQUIRE(pc.bytes_h2d >= kBytes + kBytes);

    REQUIRE(hazeFree(d) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Phase 3 — op and flush counters ([integration]; compute + genuine replay).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: op count is non-zero after recording compute ops", "[integration]") {
    const hazePerformanceCounters pc = run_add_flush_workload();
    REQUIRE(pc.op_count >= 1);
}

TEST_CASE("perf counters: flush count is non-zero after a genuine flush", "[integration]") {
    const hazePerformanceCounters pc = run_add_flush_workload();
    REQUIRE(pc.flush_count >= 1);
    // Flush timings may read zero on fast hardware; assert the ordering
    // invariant rather than a strictly positive nanosecond value.
    REQUIRE(pc.flush_time_ns_total >= pc.flush_time_ns_last);
}

TEST_CASE("perf counters: byte counters advance across a compute and readback workload",
          "[integration]") {
    const hazePerformanceCounters pc = run_add_flush_workload();
    REQUIRE(pc.bytes_h2d >= kBytes + kBytes);
    REQUIRE(pc.bytes_d2h >= kBytes);
}

TEST_CASE("perf counters: an SRP device-to-device copy increments the D2D byte counter",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);

    void *src_in = nullptr;
    void *dst_compute = nullptr;
    void *dst_copy = nullptr;
    REQUIRE(hazeMalloc(&src_in, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst_compute, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst_copy, kBytes) == HAZE_SUCCESS);

    const std::vector<uint64_t> residue = haze::test::make_residue(q, /*seed=*/42, kRingDim);
    REQUIRE(hazeMemcpy(src_in, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);

    // A compute-produced source binds dst_compute in the epoch's poly map; the
    // subsequent SRP device-to-device copy is the path that increments
    // bytes_d2d (an MRP device-to-device copy is intentionally not counted).
    REQUIRE(hazeAdd(dst_compute, src_in, src_in, /*mod_idx=*/0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(dst_copy, dst_compute, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) ==
            HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst_copy) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), dst_copy, kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) == HAZE_SUCCESS);

    const hazePerformanceCounters pc = read_counters();
    REQUIRE(pc.bytes_d2d >= kBytes);

    REQUIRE(hazeFree(dst_copy) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst_compute) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src_in) == HAZE_SUCCESS);
}
