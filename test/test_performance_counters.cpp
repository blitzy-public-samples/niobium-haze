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
//
// The two [.][concurrency] cases at the end are the deliberate exception: they
// validate the M3 thread-safety contract of the metrics aggregator under real
// contention. The first drives the public query concurrently with a live
// host-to-device workload (the user-facing guarantee). The second reaches the
// haze::metrics() singleton directly -- the only way to race snapshot() and
// reset() against live mutation, since hazeDeviceReset tears down the allocator
// and cannot safely race active device work. Both are hidden by [.] and are
// intended to run under ThreadSanitizer.

#include "core/metrics.hpp"
#include "integration_helpers.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <system_error>
#include <thread>
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

TEST_CASE("perf counters: the public query populates exactly the struct and never overflows it",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // A caller-sized snapshot followed by a sentinel guard region. The query
    // must write exactly sizeof(hazePerformanceCounters) bytes into the
    // caller's field (the M2 aligned local-snapshot + memcpy): every field is
    // overwritten to its true value and no byte past the struct is touched.
    constexpr unsigned char kSentinel = 0xAB;
    struct GuardedBuffer {
        hazePerformanceCounters pc;
        std::array<unsigned char, 64> guard;
    };
    GuardedBuffer buf{};
    std::memset(&buf, kSentinel, sizeof(buf));

    REQUIRE(hazeGetPerformanceCounters(&buf.pc) == HAZE_SUCCESS);

    // Every field written over the sentinel to its post-reset value (0); an
    // unwritten field would instead read as a non-zero 0xABAB... pattern.
    REQUIRE(buf.pc.op_count == 0);
    REQUIRE(buf.pc.bytes_h2d == 0);
    REQUIRE(buf.pc.bytes_d2h == 0);
    REQUIRE(buf.pc.bytes_d2d == 0);
    REQUIRE(buf.pc.flush_count == 0);
    REQUIRE(buf.pc.flush_time_ns_total == 0);
    REQUIRE(buf.pc.flush_time_ns_last == 0);

    // The bytes past the struct remain the sentinel: the query wrote exactly
    // one struct and never ran past the caller's field.
    for (unsigned char c : buf.guard)
        REQUIRE(c == kSentinel);
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
    // is measured. The MRP device-to-device copy is metered too (per-residue x
    // residue count) and is asserted by its own case below.
    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeMemcpy(dst_copy, dst_compute, kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE) ==
            HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.bytes_d2d - before.bytes_d2d == static_cast<uint64_t>(kBytes));
}

// ---------------------------------------------------------------------------
// MRP accounting: one high-level op regardless of residue fan-out, and MRP
// device-to-device copies metered in bytes_d2d ([integration]).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: an MRP arithmetic op increments op_count by exactly one",
          "[integration]") {
    const std::vector<uint64_t> base = haze::test::setup_integration_mrp3_config(kRingDim);

    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(base[0], /*seed=*/1, kRingDim),
        haze::test::make_residue(base[1], /*seed=*/2, kRingDim),
        haze::test::make_residue(base[2], /*seed=*/3, kRingDim),
    };
    ResiduesGuard srcs(haze::test::allocate_and_h2d_residues(inputs));
    ResiduesGuard dst(haze::test::allocate_dst_residues(base.size(), kBytes));

    // One MRP op fans out across all three residues internally but is a single
    // high-level operation: op_count must advance by exactly one, never by the
    // residue count. Snapshot after the H2D loads so only the op is measured.
    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeAddMrp(dst.ptrs().data(), haze::test::to_const(srcs.ptrs()).data(),
                       haze::test::to_const(srcs.ptrs()).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.op_count - before.op_count == 1);
    // An arithmetic op moves no bytes on its own and records no D2D copy.
    REQUIRE(after.bytes_h2d - before.bytes_h2d == 0);
    REQUIRE(after.bytes_d2d - before.bytes_d2d == 0);
}

TEST_CASE("perf counters: an MRP device-to-device copy increments bytes_d2d by the per-residue "
          "size times the residue count",
          "[integration]") {
    const std::vector<uint64_t> base = haze::test::setup_integration_mrp3_config(kRingDim);

    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(base[0], /*seed=*/4, kRingDim),
        haze::test::make_residue(base[1], /*seed=*/5, kRingDim),
        haze::test::make_residue(base[2], /*seed=*/6, kRingDim),
    };
    ResiduesGuard srcs(haze::test::allocate_and_h2d_residues(inputs));
    ResiduesGuard computed(haze::test::allocate_dst_residues(base.size(), kBytes));
    ResiduesGuard copied(haze::test::allocate_dst_residues(base.size(), kBytes));

    // Produce an MRP so the source residues are live and readable.
    REQUIRE(hazeAddMrp(computed.ptrs().data(), haze::test::to_const(srcs.ptrs()).data(),
                       haze::test::to_const(srcs.ptrs()).data(), base.data(), base.size(),
                       nullptr) == HAZE_SUCCESS);

    // Snapshot around the MRP device-to-device copy: it moves one polynomial
    // (kBytes) per residue across base.size() residues, so bytes_d2d advances
    // by exactly kBytes * residue count. The historical gap left this
    // unmetered (the SRP D2D case comment blessed the omission). A copy is not
    // a high-level op, so op_count must not move.
    const hazePerformanceCounters before = read_counters();
    REQUIRE(hazeMemcpyMrp(copied.ptrs().data(), haze::test::to_const(computed.ptrs()).data(),
                          kBytes, HAZE_MEMCPY_DEVICE_TO_DEVICE, base.data(),
                          base.size()) == HAZE_SUCCESS);
    const hazePerformanceCounters after = read_counters();

    REQUIRE(after.bytes_d2d - before.bytes_d2d ==
            static_cast<uint64_t>(kBytes) * static_cast<uint64_t>(base.size()));
    REQUIRE(after.op_count - before.op_count == 0);
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

// ---------------------------------------------------------------------------
// Snapshot consistency policy: quiescent stability, monotonicity, and internal
// coherence of a single snapshot (M3) ([integration]).
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: snapshots are quiescent-stable and every counter is monotonic",
          "[integration]") {
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim);

    // Quiescence: with no intervening work, two reads return an identical
    // coherent snapshot (the M3 mutex-guarded snapshot policy). A torn read
    // would let some fields advance relative to others between the two calls.
    const hazePerformanceCounters a = read_counters();
    const hazePerformanceCounters b = read_counters();
    REQUIRE(a.op_count == b.op_count);
    REQUIRE(a.bytes_h2d == b.bytes_h2d);
    REQUIRE(a.bytes_d2h == b.bytes_d2h);
    REQUIRE(a.bytes_d2d == b.bytes_d2d);
    REQUIRE(a.flush_count == b.flush_count);
    REQUIRE(a.flush_time_ns_total == b.flush_time_ns_total);
    REQUIRE(a.flush_time_ns_last == b.flush_time_ns_last);

    // Run a workload, then re-read: every counter is monotonic (never
    // decreases) between resets.
    record_add_flush_cycle(q, /*seed_a=*/1, /*seed_b=*/2);
    const hazePerformanceCounters c = read_counters();
    REQUIRE(c.op_count >= b.op_count);
    REQUIRE(c.bytes_h2d >= b.bytes_h2d);
    REQUIRE(c.bytes_d2h >= b.bytes_d2h);
    REQUIRE(c.bytes_d2d >= b.bytes_d2d);
    REQUIRE(c.flush_count >= b.flush_count);
    REQUIRE(c.flush_time_ns_total >= b.flush_time_ns_total);

    // A single coherent snapshot is internally consistent: at least one flush
    // has been recorded and the cumulative flush total is at least the
    // most-recent flush sample (a torn read could invert this).
    REQUIRE(c.flush_count >= 1);
    REQUIRE(c.flush_time_ns_last <= c.flush_time_ns_total);
}

// ---------------------------------------------------------------------------
// Concurrency: the metrics aggregator is thread-safe (M3). These cases are
// hidden behind the [.] tag and are intended for the HAZE_TSAN build. Code that
// runs off the main thread never calls a Catch2 assertion macro (they are not
// thread-safe): outcomes are gathered into atomics and asserted on the main
// thread after every worker has joined. A std::barrier releases all workers at
// once to maximise the overlap window.
// ---------------------------------------------------------------------------

TEST_CASE("perf counters: the public query stays coherent under a concurrent host-to-device "
          "workload",
          "[.][concurrency]") {
    configure_bytes_only();

    constexpr int kWorkers = 4;
    constexpr int kIterations = 4000;

    // One destination per worker so the concurrent copies never contend on a
    // shared shadow buffer; only the metrics leaf mutex is shared. Allocated up
    // front on the main thread so the timed window exercises H2D metering alone.
    std::vector<void *> dsts(static_cast<std::size_t>(kWorkers), nullptr);
    for (int i = 0; i < kWorkers; ++i)
        REQUIRE(hazeMalloc(&dsts[static_cast<std::size_t>(i)], kBytes) == HAZE_SUCCESS);
    ResiduesGuard dst_guard(dsts);
    const std::vector<uint64_t> host(kRingDim);

    const hazePerformanceCounters before = read_counters();
    REQUIRE(before.bytes_h2d == 0);

    std::barrier start(kWorkers + 1);
    std::atomic<int> copy_failures{0};
    std::atomic<bool> incoherent{false};

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(kWorkers) + 1);
    for (int t = 0; t < kWorkers; ++t) {
        void *dst = dsts[static_cast<std::size_t>(t)];
        workers.emplace_back([&, dst] {
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i)
                if (hazeMemcpy(dst, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) !=
                    HAZE_SUCCESS)
                    copy_failures.fetch_add(1);
        });
    }
    // Reader: every public snapshot taken during the workload must be a whole
    // multiple of kBytes (each metered H2D adds exactly kBytes under the lock,
    // so a lost or torn single-field update would surface as a fractional value)
    // and must never decrease (the counter is cumulative).
    workers.emplace_back([&] {
        start.arrive_and_wait();
        uint64_t last = 0;
        for (int i = 0; i < kWorkers * kIterations; ++i) {
            hazePerformanceCounters s{};
            if (hazeGetPerformanceCounters(&s) != HAZE_SUCCESS ||
                s.bytes_h2d % static_cast<uint64_t>(kBytes) != 0 || s.bytes_h2d < last)
                incoherent.store(true);
            last = s.bytes_h2d;
        }
    });
    for (std::thread &worker : workers)
        worker.join();

    REQUIRE(copy_failures.load() == 0);
    REQUIRE_FALSE(incoherent.load());

    // No update was lost: the cumulative H2D total advanced by exactly the sum
    // of every metered copy.
    const hazePerformanceCounters after = read_counters();
    REQUIRE(after.bytes_h2d - before.bytes_h2d == static_cast<uint64_t>(kWorkers) *
                                                      static_cast<uint64_t>(kIterations) *
                                                      static_cast<uint64_t>(kBytes));
}

TEST_CASE("perf counters: aggregator snapshot and reset stay atomic under concurrent mutation",
          "[.][concurrency]") {
    // Reaches the internal aggregator directly (see the file header): the only
    // way to race snapshot() and reset() against live mutation. Every
    // record_flush() adds a CONSTANT duration, so any coherent snapshot must
    // satisfy the cross-field invariant flush_time_ns_total == flush_count *
    // kConstNs. That invariant is preserved across an atomic reset (both fields
    // drop to zero together), so a reader that ever observes a violation has
    // caught either a torn read or a partially-applied reset.
    constexpr uint64_t kConstNs = 1000;
    constexpr int kMutators = 4;
    constexpr int kIterations = 20000;

    haze::metrics().reset();

    std::barrier start(kMutators + 2);
    std::atomic<bool> violated{false};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kMutators) + 2);
    for (int t = 0; t < kMutators; ++t)
        threads.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i)
                haze::metrics().record_flush(kConstNs);
        });
    // Reader: the cross-field coherence invariant must hold on every snapshot.
    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kIterations; ++i) {
            const hazePerformanceCounters s = haze::metrics().snapshot();
            if (s.flush_time_ns_total != s.flush_count * kConstNs)
                violated.store(true);
        }
    });
    // Resetter: periodically zero every counter as one indivisible step.
    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kIterations / 50; ++i)
            haze::metrics().reset();
    });
    for (std::thread &thread : threads)
        thread.join();

    REQUIRE_FALSE(violated.load());

    // A quiescent reset returns every counter to zero as one indivisible step.
    haze::metrics().reset();
    const hazePerformanceCounters cleared = haze::metrics().snapshot();
    REQUIRE(cleared.op_count == 0);
    REQUIRE(cleared.flush_count == 0);
    REQUIRE(cleared.flush_time_ns_total == 0);
    REQUIRE(cleared.flush_time_ns_last == 0);
}
