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
// Google Benchmark microbenchmarks for the SRP + MRP compute ops (P1b).

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

// FHE parameters shared with the integration compute tests.
constexpr uint64_t kRingDim = 4096;
constexpr uint64_t kQ0 = 576460752303415297ULL;
constexpr uint64_t kQ1 = 576460752303439873ULL;
constexpr uint64_t kQ2 = 576460752303702017ULL;
constexpr int kModIdx = 0;

// Odd automorph index in [1, 2N-1].
constexpr uint64_t kAutomorphIndex = 3;

// Bytes staged per polynomial residue (one uint64_t coefficient per slot).
constexpr auto kBytes = kRingDim * sizeof(uint64_t);

// Abort a misconfigured benchmark loudly (setup/teardown only, never timed).
void require_ok(hazeError_t rc) {
    if (rc != HAZE_SUCCESS) {
        std::println(stderr, "haze benchmark setup failed: rc={}", static_cast<int>(rc));
        std::abort();
    }
}

// Honor HAZE_TARGET (set by `make bench` / CI) when present and non-empty.
void apply_target_from_env() {
    if (const char *t = std::getenv("HAZE_TARGET"); t != nullptr && t[0] != '\0') {
        require_ok(hazeSetTarget(t));
    }
}

// Single-residue (SRP) device configuration: one ciphertext-modulus slot.
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

// Multi-residue (MRP) device configuration: three ciphertext-modulus slots.
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

// ---------------------------------------------------------------------------
// SRP benchmarks: allocate + stage once, record one op per timed iteration.
// ---------------------------------------------------------------------------

void BM_HazeAdd(benchmark::State &state) {
    setup_srp();
    void *a = nullptr;
    void *b = nullptr;
    void *d = nullptr;
    require_ok(hazeMalloc(&a, kBytes));
    require_ok(hazeMalloc(&b, kBytes));
    require_ok(hazeMalloc(&d, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    require_ok(hazeMemcpy(a, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE));
    require_ok(hazeMemcpy(b, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE));
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeAdd(d, a, b, kModIdx, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeAdd returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFree(a));
    require_ok(hazeFree(b));
    require_ok(hazeFree(d));
    require_ok(hazeDeviceReset());
}

void BM_HazeMul(benchmark::State &state) {
    setup_srp();
    void *a = nullptr;
    void *b = nullptr;
    void *d = nullptr;
    require_ok(hazeMalloc(&a, kBytes));
    require_ok(hazeMalloc(&b, kBytes));
    require_ok(hazeMalloc(&d, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    require_ok(hazeMemcpy(a, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE));
    require_ok(hazeMemcpy(b, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE));
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeMul(d, a, b, kModIdx, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeMul returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFree(a));
    require_ok(hazeFree(b));
    require_ok(hazeFree(d));
    require_ok(hazeDeviceReset());
}

void BM_HazeNTT(benchmark::State &state) {
    setup_srp();
    void *a = nullptr;
    void *d = nullptr;
    require_ok(hazeMalloc(&a, kBytes));
    require_ok(hazeMalloc(&d, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    require_ok(hazeMemcpy(a, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE));
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeNTT(d, a, kModIdx, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeNTT returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFree(a));
    require_ok(hazeFree(d));
    require_ok(hazeDeviceReset());
}

void BM_HazeAutomorph(benchmark::State &state) {
    setup_srp();
    void *a = nullptr;
    void *d = nullptr;
    require_ok(hazeMalloc(&a, kBytes));
    require_ok(hazeMalloc(&d, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    require_ok(hazeMemcpy(a, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE));
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeAutomorph(d, a, kAutomorphIndex, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeAutomorph returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFree(a));
    require_ok(hazeFree(d));
    require_ok(hazeDeviceReset());
}

// ---------------------------------------------------------------------------
// MRP benchmarks: three residues. Read operands are const void* views so the
// double-const `const void* const*` parameters bind without a const error.
// ---------------------------------------------------------------------------

void BM_HazeAddMrp(benchmark::State &state) {
    setup_mrp();
    const uint64_t base[3] = {kQ0, kQ1, kQ2};
    void *a_dev[3] = {};
    void *b_dev[3] = {};
    void *d[3] = {};
    require_ok(hazeMallocMrp(a_dev, 3, kBytes));
    require_ok(hazeMallocMrp(b_dev, 3, kBytes));
    require_ok(hazeMallocMrp(d, 3, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    const void *host_src[3] = {host.data(), host.data(), host.data()};
    require_ok(hazeMemcpyMrp(a_dev, host_src, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, 3));
    require_ok(hazeMemcpyMrp(b_dev, host_src, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, 3));
    const void *a[3] = {a_dev[0], a_dev[1], a_dev[2]};
    const void *b[3] = {b_dev[0], b_dev[1], b_dev[2]};
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeAddMrp(d, a, b, base, 3, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d[0]);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeAddMrp returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFreeMrp(a_dev, 3));
    require_ok(hazeFreeMrp(b_dev, 3));
    require_ok(hazeFreeMrp(d, 3));
    require_ok(hazeDeviceReset());
}

void BM_HazeMulMrp(benchmark::State &state) {
    setup_mrp();
    const uint64_t base[3] = {kQ0, kQ1, kQ2};
    void *a_dev[3] = {};
    void *b_dev[3] = {};
    void *d[3] = {};
    require_ok(hazeMallocMrp(a_dev, 3, kBytes));
    require_ok(hazeMallocMrp(b_dev, 3, kBytes));
    require_ok(hazeMallocMrp(d, 3, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    const void *host_src[3] = {host.data(), host.data(), host.data()};
    require_ok(hazeMemcpyMrp(a_dev, host_src, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, 3));
    require_ok(hazeMemcpyMrp(b_dev, host_src, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, 3));
    const void *a[3] = {a_dev[0], a_dev[1], a_dev[2]};
    const void *b[3] = {b_dev[0], b_dev[1], b_dev[2]};
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeMulMrp(d, a, b, base, 3, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d[0]);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeMulMrp returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFreeMrp(a_dev, 3));
    require_ok(hazeFreeMrp(b_dev, 3));
    require_ok(hazeFreeMrp(d, 3));
    require_ok(hazeDeviceReset());
}

void BM_HazeNTTMrp(benchmark::State &state) {
    setup_mrp();
    const uint64_t base[3] = {kQ0, kQ1, kQ2};
    void *a_dev[3] = {};
    void *d[3] = {};
    require_ok(hazeMallocMrp(a_dev, 3, kBytes));
    require_ok(hazeMallocMrp(d, 3, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    const void *host_src[3] = {host.data(), host.data(), host.data()};
    require_ok(hazeMemcpyMrp(a_dev, host_src, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, 3));
    const void *a[3] = {a_dev[0], a_dev[1], a_dev[2]};
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeNTTMrp(d, a, base, 3, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d[0]);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeNTTMrp returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFreeMrp(a_dev, 3));
    require_ok(hazeFreeMrp(d, 3));
    require_ok(hazeDeviceReset());
}

void BM_HazeAutomorphMrp(benchmark::State &state) {
    setup_mrp();
    const uint64_t base[3] = {kQ0, kQ1, kQ2};
    void *a_dev[3] = {};
    void *d[3] = {};
    require_ok(hazeMallocMrp(a_dev, 3, kBytes));
    require_ok(hazeMallocMrp(d, 3, kBytes));
    const std::vector<uint64_t> host(kRingDim, 1);
    const void *host_src[3] = {host.data(), host.data(), host.data()};
    require_ok(hazeMemcpyMrp(a_dev, host_src, kBytes, HAZE_MEMCPY_HOST_TO_DEVICE, base, 3));
    const void *a[3] = {a_dev[0], a_dev[1], a_dev[2]};
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
    for (auto _ : state) {
        hazeError_t rc = hazeAutomorphMrp(d, a, kAutomorphIndex, base, 3, nullptr);
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(d[0]);
        if (rc != HAZE_SUCCESS) {
            state.SkipWithError("hazeAutomorphMrp returned a non-success status");
            break;
        }
        benchmark::ClobberMemory();
    }
    require_ok(hazeFreeMrp(a_dev, 3));
    require_ok(hazeFreeMrp(d, 3));
    require_ok(hazeDeviceReset());
}

} // namespace

// Each compute benchmark runs 5 repetitions; the regression gate compares the
// median across a benchmark's iteration rows, which suppresses the per-run
// variance observed on shared CI runners. Rationale: docs/decision-log.md
// (D-43). The record->flush->replay benchmarks stay single-shot (they are
// already low-variance) and are registered in bench_record_replay.cpp.
BENCHMARK(BM_HazeAdd)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeAddMrp)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeMul)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeMulMrp)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeNTT)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeNTTMrp)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeAutomorph)->Iterations(1000)->Repetitions(5);
BENCHMARK(BM_HazeAutomorphMrp)->Iterations(1000)->Repetitions(5);
