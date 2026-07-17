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
//
// Google Benchmark microbenchmark for the record->flush->replay path (P1b).

#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdlib>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;

// Three NTT-friendly primes (q = 1 mod 2N) for N = 4096.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

constexpr int kModIdx = 0;

constexpr std::size_t kMrpResidues = 3;

// Report a benchmark error (rather than aborting the whole process) when a
// haze call did not return HAZE_SUCCESS. Returning false lets the caller stop
// the benchmark cleanly: Google Benchmark records it as errored, the
// --benchmark_out JSON stays valid, and the process exits normally — instead
// of a std::abort() that core-dumps and truncates the results file. The
// regression gate (scripts/bench_compare.py) then correctly treats an
// errored/skipped benchmark as a gate failure.
[[nodiscard]] bool check_ok(benchmark::State &state, hazeError_t rc) {
    if (rc != HAZE_SUCCESS) {
        state.SkipWithError("haze call failed with error " + std::to_string(static_cast<int>(rc)));
        return false;
    }
    return true;
}

// Apply the HAZE_TARGET selector via the explicit setter after a device reset,
// before the first configure/record. Reports the error on `state` and returns
// false if the setter fails.
[[nodiscard]] bool apply_target_from_env(benchmark::State &state) {
    if (const char *t = std::getenv("HAZE_TARGET"); t != nullptr && t[0] != '\0') {
        return check_ok(state, hazeSetTarget(t));
    }
    return true;
}

// Single-residue setup: reset, ring dimension, bridge CryptoContext init, one
// ciphertext modulus, configure device. Short-circuits on the first failure,
// reporting it on `state`; returns true only when fully configured.
[[nodiscard]] bool setup_srp(benchmark::State &state) {
    uint64_t scaffold = 0;
    return check_ok(state, hazeDeviceReset()) && apply_target_from_env(state) &&
           check_ok(state, hazeSetReducedNoise(1)) &&
           check_ok(state, hazeSetRingDimension(kRingDim)) &&
           check_ok(state, hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold)) &&
           check_ok(state, hazeSetCiphertextModulus(kModIdx, kQ0)) &&
           check_ok(state, hazeConfigureDevice());
}

// Three-prime MRP setup: all three modulus slots set before the single
// configure. Short-circuits on the first failure, reporting it on `state`.
[[nodiscard]] bool setup_mrp(benchmark::State &state) {
    uint64_t scaffold = 0;
    return check_ok(state, hazeDeviceReset()) && apply_target_from_env(state) &&
           check_ok(state, hazeSetReducedNoise(1)) &&
           check_ok(state, hazeSetRingDimension(kRingDim)) &&
           check_ok(state, hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold)) &&
           check_ok(state, hazeSetCiphertextModulus(0, kQ0)) &&
           check_ok(state, hazeSetCiphertextModulus(1, kQ1)) &&
           check_ok(state, hazeSetCiphertextModulus(2, kQ2)) &&
           check_ok(state, hazeConfigureDevice());
}

} // namespace

// Single-residue record->flush->replay cycle: stage two inputs (H2D), record
// one add, tag the output, and flush. hazeFlush is the sole materialization
// trigger; it replays the recorded epoch through the in-process simulator and
// then resets the epoch, so each iteration re-records cleanly.
void BM_RecordFlushReplaySrp(benchmark::State &state) {
    if (!setup_srp(state))
        return;
    const std::size_t bytes = kRingDim * sizeof(uint64_t);
    const std::vector<uint64_t> host(kRingDim, 1);

    void *a = nullptr;
    void *b = nullptr;
    void *d = nullptr;
    if (check_ok(state, hazeMalloc(&a, bytes)) && check_ok(state, hazeMalloc(&b, bytes)) &&
        check_ok(state, hazeMalloc(&d, bytes))) {
        for ([[maybe_unused]] auto _ : state) {
            if (!check_ok(state, hazeMemcpy(a, host.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE)) ||
                !check_ok(state, hazeMemcpy(b, host.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE)))
                break;
            hazeError_t rc = hazeAdd(d, a, b, kModIdx, nullptr);
            benchmark::DoNotOptimize(rc);
            if (!check_ok(state, rc))
                break;
            if (!check_ok(state, hazeTagOutput(d)))
                break;
            hazeError_t frc = hazeFlush();
            benchmark::DoNotOptimize(frc);
            if (!check_ok(state, frc))
                break;
            benchmark::ClobberMemory();
        }
    }

    // Best-effort teardown: a cleanup failure must neither abort the process nor
    // overwrite a benchmark error already reported above. hazeFree/hazeDeviceReset
    // are null-safe, so this is correct even if a malloc above failed.
    (void)hazeFree(a);
    (void)hazeFree(b);
    (void)hazeFree(d);
    (void)hazeDeviceReset();
}

// Three-residue (MRP) record->flush->replay cycle: stage two multi-residue
// inputs, record one add over the base, tag one residue (which tags the whole
// group), and flush. Flush self-bounds the epoch as in the single-residue case.
void BM_RecordFlushReplayMrp(benchmark::State &state) {
    if (!setup_mrp(state))
        return;
    const uint64_t base[kMrpResidues] = {kQ0, kQ1, kQ2};
    const std::size_t bytes = kRingDim * sizeof(uint64_t);
    const std::vector<uint64_t> host(kRingDim, 1);

    void *a[kMrpResidues] = {};
    void *b[kMrpResidues] = {};
    void *d[kMrpResidues] = {};
    if (check_ok(state, hazeMallocMrp(a, kMrpResidues, bytes)) &&
        check_ok(state, hazeMallocMrp(b, kMrpResidues, bytes)) &&
        check_ok(state, hazeMallocMrp(d, kMrpResidues, bytes))) {
        const void *host_src[kMrpResidues] = {host.data(), host.data(), host.data()};

        for ([[maybe_unused]] auto _ : state) {
            if (!check_ok(state, hazeMemcpyMrp(a, host_src, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base,
                                               kMrpResidues)) ||
                !check_ok(state, hazeMemcpyMrp(b, host_src, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base,
                                               kMrpResidues)))
                break;
            hazeError_t rc = hazeAddMrp(d, a, b, base, kMrpResidues, nullptr);
            benchmark::DoNotOptimize(rc);
            if (!check_ok(state, rc))
                break;
            if (!check_ok(state, hazeTagOutput(d[0])))
                break;
            hazeError_t frc = hazeFlush();
            benchmark::DoNotOptimize(frc);
            if (!check_ok(state, frc))
                break;
            benchmark::ClobberMemory();
        }
    }

    // Best-effort teardown (see BM_RecordFlushReplaySrp). hazeFreeMrp tolerates
    // null residue slots, so this is safe even if a malloc above failed.
    (void)hazeFreeMrp(a, kMrpResidues);
    (void)hazeFreeMrp(b, kMrpResidues);
    (void)hazeFreeMrp(d, kMrpResidues);
    (void)hazeDeviceReset();
}

BENCHMARK(BM_RecordFlushReplaySrp)->Iterations(100);
BENCHMARK(BM_RecordFlushReplayMrp)->Iterations(100);
