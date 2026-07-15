// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Tests for hazeGetPerformanceCounters (P2b), the public performance-metrics
// surface. It writes a hazePerformanceCounters snapshot of cumulative runtime
// counters: op_count and the flush counters advance through the
// record -> hazeFlush -> replay path, while the byte counters (H2D / D2H / D2D)
// advance synchronously at each hazeMemcpy. hazeDeviceReset zeroes every
// counter, including from a previously dirtied state. Flush timings are
// wall-clock nanoseconds and may legitimately read zero on fast hardware; the
// invariant asserted is total == last after one flush and
// total == prior_total + last after a second flush.
//
// Counters are read exclusively through the public query so the tests are
// executable evidence for the metrics surface itself, not for an internal
// aggregator. Assertions use exact before/after deltas rather than lower
// bounds so double-counting or spurious increments are caught.

#include "integration_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;

// Scope guard that frees a single hazeMalloc'd device pointer on scope exit so
// a REQUIRE failure mid-case cannot leak the allocation.
class DeviceGuard {
  public:
    explicit DeviceGuard(void *ptr) noexcept : ptr_(ptr) {}
    DeviceGuard(const DeviceGuard &) = delete;
    DeviceGuard &operator=(const DeviceGuard &) = delete;
    DeviceGuard(DeviceGuard &&) = delete;
    DeviceGuard &operator=(DeviceGuard &&) = delete;
    ~DeviceGuard() {
        if (ptr_ != nullptr)
            (void)hazeFree(ptr_);
    }
    void *get() const noexcept { return ptr_; }

  private:
    void *ptr_ = nullptr;
};

// Scope guard for a vector of hazeMalloc'd residue pointers. The destructor is
// exception-safe: it frees each pointer directly and ignores the result rather
// than routing through free_all_residues, whose REQUIRE could throw during
// unwinding.
class ResiduesGuard {
  public:
    explicit ResiduesGuard(std::vector<void *> ptrs) noexcept : ptrs_(std::move(ptrs)) {}
    ResiduesGuard(const ResiduesGuard &) = delete;
    ResiduesGuard &operator=(const ResiduesGuard &) = delete;
    ResiduesGuard(ResiduesGuard &&) = delete;
    ResiduesGuard &operator=(ResiduesGuard &&) = delete;
    ~ResiduesGuard() {
        for (void *p : ptrs_)
            if (p != nullptr)
                (void)hazeFree(p);
    }
    const std::vector<void *> &ptrs() const noexcept { return ptrs_; }

  private:
    std::vector<void *> ptrs_;
};

// Read the current counter snapshot through the public query. Aborts the
// enclosing Catch2 assertion scope if the query itself fails.
hazePerformanceCounters read_counters() {
    hazePerformanceCounters pc{};
    REQUIRE(hazeGetPerformanceCounters(&pc) == HAZE_SUCCESS);
    return pc;
}

// One record -> tag -> flush -> D2H cycle assuming the crypto config is already
// established. Moves 2 * kBytes host-to-device, emits one op, performs one
// genuine flush, and moves kBytes device-to-host. Cleans up via RAII.
void record_add_flush_cycle(uint64_t q, uint64_t seed_a, uint64_t seed_b) {
    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(q, seed_a, kRingDim),
        haze::test::make_residue(q, seed_b, kRingDim),
    };
    ResiduesGuard d_in(haze::test::allocate_and_h2d_residues(inputs));
    ResiduesGuard d_dst(haze::test::allocate_dst_residues(/*count=*/1, kBytes));

    REQUIRE(hazeAdd(d_dst.ptrs()[0], d_in.ptrs()[0], d_in.ptrs()[1], /*mod_idx=*/0, nullptr) ==
            HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst.ptrs()[0]) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    std::vector<uint64_t> out(kRingDim);
    REQUIRE(hazeMemcpy(out.data(), d_dst.ptrs()[0], kBytes, HAZE_MEMCPY_DEVICE_TO_HOST) ==
            HAZE_SUCCESS);
}

// Reset + ring dimension + configure, enough to hazeMalloc / hazeMemcpy device
// buffers without a full crypto context (used by the byte-counter cases).
void configure_bytes_only() {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);
}

} // namespace

// ---------------------------------------------------------------------------
// Argument validation, struct layout, and reset semantics ([unit]).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: hazeGetPerformanceCounters rejects a null pointer", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetPerformanceCounters(nullptr) == HAZE_ERROR_INVALID_VALUE);
    hazeGetLastError();
}

TEST_CASE("perf counters: the counters struct has the documented ABI layout", "[unit]") {
    // Pin the exact field offsets and total size so an accidental reorder or
    // insertion (which would silently break the C ABI) fails to compile.
    STATIC_REQUIRE(sizeof(hazePerformanceCounters) == 7 * sizeof(uint64_t));
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, op_count) == 0);
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, bytes_h2d) == 8);
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, bytes_d2h) == 16);
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, bytes_d2d) == 24);
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, flush_count) == 32);
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, flush_time_ns_total) == 40);
    STATIC_REQUIRE(offsetof(hazePerformanceCounters, flush_time_ns_last) == 48);
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
// Synchronous byte counters, exact deltas ([integration]; no flush required).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: a host-to-device copy increments bytes_h2d by exactly the copy size",
          "[integration]") {
    configure_bytes_only();
    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);
    DeviceGuard d_guard(d);
    const std::vector<uint64_t> host(kRingDim);

    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeMemcpy(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.bytes_h2d - before.bytes_h2d == static_cast<uint64_t>(kBytes));
    // A pure H2D moves no other bytes and emits no op or flush.
    REQUIRE(after.bytes_d2h - before.bytes_d2h == 0);
    REQUIRE(after.bytes_d2d - before.bytes_d2d == 0);
    REQUIRE(after.op_count - before.op_count == 0);
    REQUIRE(after.flush_count - before.flush_count == 0);
}

TEST_CASE("perf counters: two host-to-device copies accumulate exactly twice the copy size",
          "[integration]") {
    configure_bytes_only();
    void *d = nullptr;
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);
    DeviceGuard d_guard(d);
    const std::vector<uint64_t> host(kRingDim);

    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeMemcpy(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.bytes_h2d - before.bytes_h2d == 2U * static_cast<uint64_t>(kBytes));
}

// ---------------------------------------------------------------------------
// Op and flush counters, exact deltas ([integration]; compute + genuine replay).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: recording one compute op increments op_count by exactly one",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);
    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(q, /*seed=*/1, kRingDim),
        haze::test::make_residue(q, /*seed=*/2, kRingDim),
    };
    ResiduesGuard d_in(haze::test::allocate_and_h2d_residues(inputs));
    ResiduesGuard d_dst(haze::test::allocate_dst_residues(/*count=*/1, kBytes));

    // Snapshot after the inputs are uploaded so only the add is measured.
    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeAdd(d_dst.ptrs()[0], d_in.ptrs()[0], d_in.ptrs()[1], /*mod_idx=*/0, nullptr) ==
            HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.op_count - before.op_count == 1);
    // Recording an op moves no host/device bytes on its own.
    REQUIRE(after.bytes_h2d - before.bytes_h2d == 0);
}

TEST_CASE("perf counters: a single flush increments flush_count by one and total equals last",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);
    record_add_flush_cycle(q, /*seed_a=*/1, /*seed_b=*/2);
    const hazePerformanceCounters pc = read_counters();

    REQUIRE(pc.flush_count == 1);
    // Exactly one flush: cumulative total is the single last measurement.
    REQUIRE(pc.flush_time_ns_total == pc.flush_time_ns_last);
}

TEST_CASE("perf counters: an SRP device-to-device copy increments bytes_d2d by exactly the "
          "copy size",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);

    void *src_in = nullptr;
    void *dst_compute = nullptr;
    void *dst_copy = nullptr;
    REQUIRE(hazeMalloc(&src_in, kBytes) == HAZE_SUCCESS);
    DeviceGuard src_in_guard(src_in);
    REQUIRE(hazeMalloc(&dst_compute, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_compute_guard(dst_compute);
    REQUIRE(hazeMalloc(&dst_copy, kBytes) == HAZE_SUCCESS);
    DeviceGuard dst_copy_guard(dst_copy);

    const std::vector<uint64_t> residue = haze::test::make_residue(q, /*seed=*/42, kRingDim);
    REQUIRE(hazeMemcpy(src_in, residue.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst_compute, src_in, src_in, /*mod_idx=*/0, nullptr) == HAZE_SUCCESS);

    // Snapshot around the SRP device-to-device copy so only its byte movement
    // is measured (an MRP device-to-device copy is intentionally not counted).
    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeMemcpy(dst_copy, dst_compute, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) ==
            HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.bytes_d2d - before.bytes_d2d == static_cast<uint64_t>(kBytes));
}

// ---------------------------------------------------------------------------
// The public metrics surface reports a known workload exactly (R4 evidence).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: the public query reports the exact workload totals", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);
    record_add_flush_cycle(q, /*seed_a=*/1, /*seed_b=*/2);

    // hazeGetPerformanceCounters is the public metrics endpoint. After one
    // add-flush cycle from a reset state the observable totals are exact:
    // two H2D inputs, one op, one flush, one D2H readback, no D2D.
    const hazePerformanceCounters pc = read_counters();
    REQUIRE(pc.op_count == 1);
    REQUIRE(pc.bytes_h2d == 2U * static_cast<uint64_t>(kBytes));
    REQUIRE(pc.bytes_d2h == static_cast<uint64_t>(kBytes));
    REQUIRE(pc.bytes_d2d == 0);
    REQUIRE(pc.flush_count == 1);
    REQUIRE(pc.flush_time_ns_total == pc.flush_time_ns_last);
}

// ---------------------------------------------------------------------------
// Reset from a dirty state, failed ops, write-vs-flush, and multi-flush totals.
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: device reset zeroes counters from a dirtied state", "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);
    record_add_flush_cycle(q, /*seed_a=*/1, /*seed_b=*/2);

    // Counters are dirty (non-zero) after the workload.
    const hazePerformanceCounters dirty = read_counters();
    REQUIRE(dirty.op_count > 0);
    REQUIRE(dirty.bytes_h2d > 0);
    REQUIRE(dirty.flush_count > 0);

    // Reset must return every counter to zero, not merely leave a fresh state.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazePerformanceCounters cleared = read_counters();
    REQUIRE(cleared.op_count == 0);
    REQUIRE(cleared.bytes_h2d == 0);
    REQUIRE(cleared.bytes_d2h == 0);
    REQUIRE(cleared.bytes_d2d == 0);
    REQUIRE(cleared.flush_count == 0);
    REQUIRE(cleared.flush_time_ns_total == 0);
    REQUIRE(cleared.flush_time_ns_last == 0);
}

TEST_CASE("perf counters: a failed op does not increment op_count", "[integration]") {
    haze::test::setup_integration_compute_config(kRingDim);

    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard d_dst_guard(d_dst);

    // Synthetic device addresses that were never allocated. The int-to-ptr is
    // deliberate: the add must be rejected before any op is emitted.
    // NOLINTBEGIN(performance-no-int-to-ptr)
    void *fake1 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x8000000ULL);
    void *fake2 = reinterpret_cast<void *>(uintptr_t{0x4000000000ULL} + 0x9000000ULL);
    // NOLINTEND(performance-no-int-to-ptr)

    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeAdd(d_dst, fake1, fake2, /*mod_idx=*/0, nullptr) == HAZE_ERROR_UNKNOWN_ADDRESS);
    hazeGetLastError();
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.op_count - before.op_count == 0);
}

TEST_CASE("perf counters: hazeWriteProgram records without incrementing flush_count",
          "[integration]") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "haze_perf_counters_write_program";
    std::error_code ec;
    fs::remove_all(dir, ec);

    // Program directory must be set before the first compute (forwarded at
    // init), so this case builds its config explicitly rather than via the
    // shared helper.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramDirectory(dir.string().c_str()) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kRingDim) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, picked) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *d_a = nullptr;
    void *d_b = nullptr;
    void *d_dst = nullptr;
    REQUIRE(hazeMalloc(&d_a, kBytes) == HAZE_SUCCESS);
    DeviceGuard d_a_guard(d_a);
    REQUIRE(hazeMalloc(&d_b, kBytes) == HAZE_SUCCESS);
    DeviceGuard d_b_guard(d_b);
    REQUIRE(hazeMalloc(&d_dst, kBytes) == HAZE_SUCCESS);
    DeviceGuard d_dst_guard(d_dst);

    const std::vector<uint64_t> a(kRingDim, 3);
    const std::vector<uint64_t> b(kRingDim, 4);
    REQUIRE(hazeMemcpy(d_a, a.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(d_b, b.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d_dst, d_a, d_b, /*mod_idx=*/0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d_dst) == HAZE_SUCCESS);

    // Writing the project trace is not a replay: flush_count must not advance,
    // and no new op is emitted by the write itself.
    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeWriteProgram() == HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.flush_count - before.flush_count == 0);
    REQUIRE(after.op_count - before.op_count == 0);

    fs::remove_all(dir, ec);
}

TEST_CASE("perf counters: a second flush advances flush_count and totals exactly",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);

    record_add_flush_cycle(q, /*seed_a=*/1, /*seed_b=*/2);
    const hazePerformanceCounters after_one = read_counters();
    REQUIRE(after_one.flush_count == 1);
    REQUIRE(after_one.flush_time_ns_total == after_one.flush_time_ns_last);

    record_add_flush_cycle(q, /*seed_a=*/3, /*seed_b=*/4);
    const hazePerformanceCounters after_two = read_counters();

    // Two flushes: count advances by one, and the cumulative total is the prior
    // total plus the most-recent flush time.
    REQUIRE(after_two.flush_count == 2);
    REQUIRE(after_two.flush_time_ns_total ==
            after_one.flush_time_ns_total + after_two.flush_time_ns_last);
    // Two full cycles moved four H2D inputs and emitted two ops.
    REQUIRE(after_two.bytes_h2d == 4U * static_cast<uint64_t>(kBytes));
    REQUIRE(after_two.op_count == 2);
}
