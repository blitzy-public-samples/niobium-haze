// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Regression coverage for haze::extract_polynomial_values (src/core/
// polynomial_io.cpp), which reads the integer component values out of an
// opaque fhetch::Polynomial on the materialization/replay path.
//
// The values are read directly from the polynomial's in-memory representation
// (via fhetch::Polynomial::int_data()), with no temporary scratch file or
// serialization round-trip, so extraction holds no shared filesystem state.
// These cases pin two properties: (1) a single extraction returns exactly the
// stored components, and (2) concurrent extractions that reuse the SAME output
// tag from many threads never fail and never read back one another's values.
// Property (2) is a standing regression guard: an earlier implementation
// staged the read-back through a predictable temp file and raced on it under a
// shared temp directory, surfacing on the noexcept C ABI as HAZE_ERROR_INTERNAL.
// The concurrency cases run in the DEFAULT suite -- NOT behind [.] -- so any
// future reintroduction of shared scratch state fails CI immediately. See
// decision log D-42 for the rationale.

#include "core/polynomial_io.hpp"

#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <niobium/fhetch_api.h>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fhetch = niobium::fhetch;

// Build a valid integer polynomial whose components are a known, distinct
// sequence (base + i) so any cross-call contamination shows up as a value
// mismatch rather than only as an outright decode failure.
fhetch::Polynomial make_poly(uint64_t ring_dim, uint64_t base) {
    std::vector<uint64_t> components(ring_dim);
    for (uint64_t i = 0; i < ring_dim; ++i)
        components[i] = base + i;
    return fhetch::Polynomial::from_data(components, ring_dim, fhetch::Format::Evaluation);
}

// Build a valid integer polynomial whose components are all `fill`.
fhetch::Polynomial constant_poly(uint64_t fill, uint64_t ring_dim) {
    std::vector<uint64_t> components(ring_dim, fill);
    return fhetch::Polynomial::from_data(components, ring_dim, fhetch::Format::Coefficient);
}

} // namespace

TEST_CASE("polynomial_io: extract_polynomial_values round-trips integer components", "[unit]") {
    constexpr uint64_t kRingDim = 8;
    const fhetch::Polynomial p = make_poly(kRingDim, 100);

    std::vector<uint64_t> expected(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        expected[i] = 100 + i;

    // A single extraction returns exactly the stored components.
    std::vector<uint64_t> out;
    REQUIRE(haze::extract_polynomial_values(p, "roundtrip", out));
    REQUIRE(out == expected);

    // Reusing the same tag on a second call still succeeds: extraction reads
    // straight from the in-memory polynomial and keeps no per-call state, so a
    // prior call cannot poison a later one.
    std::vector<uint64_t> out2;
    REQUIRE(haze::extract_polynomial_values(p, "roundtrip", out2));
    REQUIRE(out2 == expected);
}

TEST_CASE("extract_polynomial_values round-trips integer components", "[unit][polynomial_io]") {
    constexpr uint64_t kRingDim = 64;
    std::vector<uint64_t> expected(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i)
        expected[i] = (i * 7U) + 1U;
    const auto poly =
        fhetch::Polynomial::from_data(expected, kRingDim, fhetch::Format::Coefficient);

    std::vector<uint64_t> out;
    REQUIRE(haze::extract_polynomial_values(poly, "haze_out_0", out));
    REQUIRE(out == expected);
}

TEST_CASE("polynomial_io: concurrent same-tag extraction is collision-free", "[unit]") {
    constexpr uint64_t kRingDim = 8;
    constexpr int kWorkers = 8;
    constexpr int kIterations = 16;

    // Every worker uses the IDENTICAL tag on purpose: that is the exact input
    // that produced a shared filename under the old implementation. Each worker
    // owns a polynomial with a distinct component base, so reading another
    // worker's values would surface as a value mismatch, not just a failure.
    constexpr std::string_view kSharedTag = "concurrent";

    std::atomic<int> extract_failures{0};
    std::atomic<int> value_mismatches{0};
    std::barrier start(kWorkers);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(kWorkers));
    for (int t = 0; t < kWorkers; ++t) {
        const uint64_t base = static_cast<uint64_t>(t + 1) * 1000ULL;
        workers.emplace_back([&, base] {
            const fhetch::Polynomial p = make_poly(kRingDim, base);
            std::vector<uint64_t> expected(kRingDim);
            for (uint64_t i = 0; i < kRingDim; ++i)
                expected[i] = base + i;

            // Release all workers simultaneously to maximise the overlap window
            // in which shared read-back state would collide.
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i) {
                std::vector<uint64_t> out;
                if (!haze::extract_polynomial_values(p, kSharedTag, out)) {
                    extract_failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (out != expected)
                    value_mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();

    REQUIRE(extract_failures.load() == 0);
    REQUIRE(value_mismatches.load() == 0);
}

TEST_CASE("extract_polynomial_values is collision-safe under concurrent same-tag extraction",
          "[unit][polynomial_io]") {
    // Each thread extracts a DISTINCT-valued polynomial under the SAME output
    // tag. Extraction reads each polynomial's components directly from memory
    // and holds no shared filesystem state, so every thread must read back its
    // OWN values on every iteration. Catch2 assertion macros are not
    // thread-safe, so each worker records its mismatch count into a disjoint
    // slot and the assertions run on the main thread after join.
    constexpr uint64_t kRingDim = 64;
    constexpr std::size_t kThreads = 8;
    constexpr int kIters = 250;

    std::vector<int> mismatches(kThreads, 0);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (std::size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &mismatches]() {
            const uint64_t fill = static_cast<uint64_t>(t) + 1U;
            const std::vector<uint64_t> expected(kRingDim, fill);
            const auto poly = constant_poly(fill, kRingDim);

            int local = 0;
            for (int i = 0; i < kIters; ++i) {
                std::vector<uint64_t> out;
                // Identical tag for every thread exercises the shared-name path.
                if (!haze::extract_polynomial_values(poly, "haze_out_0", out) || out != expected) {
                    ++local;
                }
            }
            mismatches[t] = local;
        });
    }

    for (auto &worker : workers)
        worker.join();

    int total = 0;
    for (const int m : mismatches)
        total += m;
    REQUIRE(total == 0);
}
