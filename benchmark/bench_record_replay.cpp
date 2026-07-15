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
#include <cstdio> // IWYU pragma: keep
#include <cstdlib>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <print>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;

// Three NTT-friendly primes (q = 1 mod 2N) for N = 4096.
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;

constexpr int kModIdx = 0;

constexpr std::size_t kMrpResidues = 3;

// Abort the process if a haze call did not return HAZE_SUCCESS.
void require_ok(hazeError_t rc) {
    if (rc != HAZE_SUCCESS) {
        std::println(stderr, "haze call failed with error {}", static_cast<int>(rc));
        std::abort();
    }
}

// Apply the HAZE_TARGET selector via the explicit setter after a device reset,
// before the first configure/record.
void apply_target_from_env() {
    if (const char *t = std::getenv("HAZE_TARGET"); t != nullptr && t[0] != '\0') {
        require_ok(hazeSetTarget(t));
    }
}

// Single-residue setup: reset, ring dimension, bridge CryptoContext init, one
// ciphertext modulus, configure device.
void setup_srp() {
    require_ok(hazeDeviceReset());
    apply_target_from_env();
    require_ok(hazeSetReducedNoise(1));
    require_ok(hazeSetRingDimension(kRingDim));
    uint64_t scaffold = 0;
    require_ok(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold));
    require_ok(hazeSetCiphertextModulus(kModIdx, kQ0));
    require_ok(hazeConfigureDevice());
}

// Three-prime MRP setup: all three modulus slots set before the single
// configure.
void setup_mrp() {
    require_ok(hazeDeviceReset());
    apply_target_from_env();
    require_ok(hazeSetReducedNoise(1));
    require_ok(hazeSetRingDimension(kRingDim));
    uint64_t scaffold = 0;
    require_ok(hazeReplayBridgeInitCryptoContext(kRingDim, kQ0, &scaffold));
    require_ok(hazeSetCiphertextModulus(0, kQ0));
    require_ok(hazeSetCiphertextModulus(1, kQ1));
    require_ok(hazeSetCiphertextModulus(2, kQ2));
    require_ok(hazeConfigureDevice());
}

} // namespace

// Single-residue record->flush->replay cycle: stage two inputs (H2D), record
// one add, tag the output, and flush. hazeFlush is the sole materialization
// trigger; it replays the recorded epoch through the in-process simulator and
// then resets the epoch, so each iteration re-records cleanly.
void BM_RecordFlushReplaySrp(benchmark::State &state) {
    setup_srp();
    const std::size_t bytes = kRingDim * sizeof(uint64_t);
    const std::vector<uint64_t> host(kRingDim, 1);

    void *a = nullptr;
    void *b = nullptr;
    void *d = nullptr;
    require_ok(hazeMalloc(&a, bytes));
    require_ok(hazeMalloc(&b, bytes));
    require_ok(hazeMalloc(&d, bytes));

    for ([[maybe_unused]] auto _ : state) {
        require_ok(hazeMemcpy(a, host.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE));
        require_ok(hazeMemcpy(b, host.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE));
        hazeError_t rc = hazeAdd(d, a, b, kModIdx, nullptr);
        benchmark::DoNotOptimize(rc);
        require_ok(rc);
        require_ok(hazeTagOutput(d));
        hazeError_t frc = hazeFlush();
        benchmark::DoNotOptimize(frc);
        require_ok(frc);
        benchmark::ClobberMemory();
    }

    require_ok(hazeFree(a));
    require_ok(hazeFree(b));
    require_ok(hazeFree(d));
    require_ok(hazeDeviceReset());
}

// Three-residue (MRP) record->flush->replay cycle: stage two multi-residue
// inputs, record one add over the base, tag one residue (which tags the whole
// group), and flush. Flush self-bounds the epoch as in the single-residue case.
void BM_RecordFlushReplayMrp(benchmark::State &state) {
    setup_mrp();
    const uint64_t base[kMrpResidues] = {kQ0, kQ1, kQ2};
    const std::size_t bytes = kRingDim * sizeof(uint64_t);
    const std::vector<uint64_t> host(kRingDim, 1);

    void *a[kMrpResidues] = {};
    void *b[kMrpResidues] = {};
    void *d[kMrpResidues] = {};
    require_ok(hazeMallocMrp(a, kMrpResidues, bytes));
    require_ok(hazeMallocMrp(b, kMrpResidues, bytes));
    require_ok(hazeMallocMrp(d, kMrpResidues, bytes));

    const void *host_src[kMrpResidues] = {host.data(), host.data(), host.data()};

    for ([[maybe_unused]] auto _ : state) {
        require_ok(
            hazeMemcpyMrp(a, host_src, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, kMrpResidues));
        require_ok(
            hazeMemcpyMrp(b, host_src, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, kMrpResidues));
        hazeError_t rc = hazeAddMrp(d, a, b, base, kMrpResidues, nullptr);
        benchmark::DoNotOptimize(rc);
        require_ok(rc);
        require_ok(hazeTagOutput(d[0]));
        hazeError_t frc = hazeFlush();
        benchmark::DoNotOptimize(frc);
        require_ok(frc);
        benchmark::ClobberMemory();
    }

    require_ok(hazeFreeMrp(a, kMrpResidues));
    require_ok(hazeFreeMrp(b, kMrpResidues));
    require_ok(hazeFreeMrp(d, kMrpResidues));
    require_ok(hazeDeviceReset());
}

BENCHMARK(BM_RecordFlushReplaySrp)->Iterations(100);
BENCHMARK(BM_RecordFlushReplayMrp)->Iterations(100);
