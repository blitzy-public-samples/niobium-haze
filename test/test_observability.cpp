// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Runtime verification for the library-context Observability pillars that the
// record-and-replay runtime is required to deliver: functional correlation IDs
// and op/epoch span tracing across the record -> flush -> replay pipeline and
// the replay_bridge boundary (see docs/observability/README.md and the
// observability decisions in docs/decision-log.md).
//
// These cases exercise the *runtime* behaviour, not merely the presence of
// helper functions:
//
//   * The pure-unit cases prove, deterministically and without the simulator,
//     that an enabled TraceSpan emits exactly one escaped, line-atomic record
//     per edge (begin on construction, end on destruction) stamped with the
//     current thread-local correlation id; that tracing is OFF by default and
//     then emits nothing; that the HAZE_TRACE toggle round-trips (trace_enabled()
//     re-reads the environment on each call); and that the correlation id is
//     thread-local.
//
//   * The integration cases drive a real record -> hazeFlush -> replay cycle
//     with tracing enabled, capture stderr, and assert that the emitted flush
//     span carries a NON-ZERO correlation id (the epoch wired its id onto the
//     logging register -- the defect this test guards against was cid=0 on every
//     line), that the id is stable within an epoch, that a second epoch gets a
//     distinct id, and that with tracing OFF a full cycle emits no trace records
//     (no regression to the default-quiet stderr surface).

#include "common/log.hpp"
#include "integration_helpers.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kRingDim = 4096;
constexpr std::size_t kBytes = kRingDim * sizeof(uint64_t);
constexpr uint64_t kQ0 = 576460752303415297ULL;

// Enable / disable span tracing through the HAZE_TRACE environment variable,
// which trace_enabled() re-reads on every call, so the toggle takes effect
// immediately for any span constructed afterwards.
void enable_trace() {
    setenv("HAZE_TRACE", "1", 1); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
}
void disable_trace() {
    unsetenv("HAZE_TRACE"); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
}

// Runs `emit` with stderr redirected to an in-memory temp file and returns the
// bytes it wrote, restoring the real stderr before returning. Mirrors the
// helper in test_documented_noops.cpp. `emit` must not throw: an exception
// would escape before stderr is restored. Best-effort: if capture cannot be set
// up, `emit` still runs and an empty string is returned.
template <typename F> std::string capture_stderr(const F &emit) {
    std::string out;
    std::fflush(stderr);
    std::FILE *cap = std::tmpfile();
    if (cap == nullptr) {
        emit();
        return out;
    }
    const int stderr_fd = fileno(stderr); // NOLINT(misc-include-cleaner)
    const int saved = dup(stderr_fd);     // NOLINT(misc-include-cleaner)
    if (saved < 0) {
        (void)std::fclose(cap);
        emit();
        return out;
    }
    (void)dup2(fileno(cap), stderr_fd); // NOLINT(misc-include-cleaner)
    emit();
    std::fflush(stderr);
    (void)dup2(saved, stderr_fd); // NOLINT(misc-include-cleaner)
    (void)close(saved);           // NOLINT(misc-include-cleaner)

    if (std::fseek(cap, 0, SEEK_END) == 0) {
        const long size = std::ftell(cap);
        if (size > 0 && std::fseek(cap, 0, SEEK_SET) == 0) {
            out.resize(static_cast<std::size_t>(size));
            const std::size_t got = std::fread(out.data(), 1, out.size(), cap);
            out.resize(got);
        }
    }
    (void)std::fclose(cap);
    return out;
}

// Restores both the process-wide HAZE_TRACE toggle and the thread-local
// correlation id on scope exit so a case cannot leak observability state into
// the next one.
class ObservabilityStateGuard {
  public:
    ObservabilityStateGuard() noexcept : cid_(haze::current_correlation_id()) {
        const char *v = std::getenv("HAZE_TRACE");
        had_trace_ = v != nullptr;
        if (had_trace_)
            prev_trace_ = v;
    }
    ObservabilityStateGuard(const ObservabilityStateGuard &) = delete;
    ObservabilityStateGuard &operator=(const ObservabilityStateGuard &) = delete;
    ObservabilityStateGuard(ObservabilityStateGuard &&) = delete;
    ObservabilityStateGuard &operator=(ObservabilityStateGuard &&) = delete;
    ~ObservabilityStateGuard() {
        if (had_trace_)
            setenv("HAZE_TRACE", prev_trace_.c_str(), 1); // NOLINT(misc-include-cleaner)
        else
            unsetenv("HAZE_TRACE"); // NOLINT(misc-include-cleaner)
        haze::set_correlation_id(cid_);
    }

  private:
    bool had_trace_ = false;
    std::string prev_trace_;
    std::uint64_t cid_;
};

// One parsed "[haze] [cid=<id>] <detail>" record, where <detail> is the
// "<tag>: <body>" payload (e.g. "trace.span.begin: epoch.flush").
struct TraceRecord {
    std::uint64_t cid = 0;
    std::string detail;
};

// Splits a captured stderr blob into individual span-trace records. Only lines
// that match the documented framing and whose payload is a "trace.span.*"
// record are returned; any other output is ignored.
std::vector<TraceRecord> collect_trace_records(const std::string &captured) {
    std::vector<TraceRecord> records;
    std::size_t pos = 0;
    while (pos < captured.size()) {
        const std::size_t nl = captured.find('\n', pos);
        const std::size_t end = (nl == std::string::npos) ? captured.size() : nl;
        const std::string line = captured.substr(pos, end - pos);
        pos = (nl == std::string::npos) ? captured.size() : nl + 1;

        static constexpr char kCidMarker[] = "[haze] [cid=";
        static constexpr char kClose[] = "] ";
        if (!line.starts_with(kCidMarker))
            continue;
        const std::size_t cid_start = sizeof(kCidMarker) - 1;
        const std::size_t close_at = line.find(kClose, cid_start);
        if (close_at == std::string::npos)
            continue;
        std::string detail = line.substr(close_at + (sizeof(kClose) - 1));
        // Only span-trace records participate; error diagnostics and any other
        // tagged line are ignored.
        if (!detail.starts_with("trace.span."))
            continue;

        TraceRecord rec;
        rec.cid = static_cast<std::uint64_t>(std::strtoull(line.c_str() + cid_start, nullptr, 10));
        rec.detail = std::move(detail);
        records.push_back(std::move(rec));
    }
    return records;
}

// True if any record's detail equals `text`.
bool any_detail_is(const std::vector<TraceRecord> &records, const std::string &text) {
    return std::ranges::any_of(records, [&](const TraceRecord &r) { return r.detail == text; });
}

// Records one add -> tag -> flush cycle with tracing enabled and returns the
// captured trace records. The crypto config, allocations, and H2D uploads run
// before the capture window (so recording is already open and its correlation
// id assigned); only the compute op, output tag, and flush -- the phases that
// emit trace records -- run inside the window. No Catch2 REQUIRE runs inside the
// capture lambda, so it cannot throw with stderr redirected.
std::vector<TraceRecord> capture_add_flush_traces(uint64_t q, uint64_t seed_a, uint64_t seed_b) {
    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(q, seed_a, kRingDim),
        haze::test::make_residue(q, seed_b, kRingDim),
    };
    const std::vector<void *> d_in = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(/*count=*/1, kBytes);

    enable_trace();
    hazeError_t add_rc = HAZE_ERROR_INTERNAL;
    hazeError_t tag_rc = HAZE_ERROR_INTERNAL;
    hazeError_t flush_rc = HAZE_ERROR_INTERNAL;
    const std::string captured = capture_stderr([&]() {
        add_rc = hazeAdd(d_dst[0], d_in[0], d_in[1], /*mod_idx=*/0, nullptr);
        tag_rc = hazeTagOutput(d_dst[0]);
        flush_rc = hazeFlush();
    });
    disable_trace();

    REQUIRE(add_rc == HAZE_SUCCESS);
    REQUIRE(tag_rc == HAZE_SUCCESS);
    REQUIRE(flush_rc == HAZE_SUCCESS);

    haze::test::free_all_residues(d_in);
    haze::test::free_all_residues(d_dst);
    return collect_trace_records(captured);
}

} // namespace

// ---------------------------------------------------------------------------
// Pure-unit evidence for the tracing surface (no simulator required).
// ---------------------------------------------------------------------------

TEST_CASE("observability: HAZE_TRACE toggles trace_enabled", "[unit]") {
    const ObservabilityStateGuard restore;
    enable_trace();
    REQUIRE(haze::trace_enabled());
    disable_trace();
    REQUIRE_FALSE(haze::trace_enabled());
}

TEST_CASE("observability: an enabled span emits begin/end stamped with the current correlation id",
          "[unit]") {
    const ObservabilityStateGuard restore;
    enable_trace();
    // 0x5150 == 20816 decimal; used to check the exact stamped id.
    haze::set_correlation_id(0x5150ULL);

    const std::string captured = capture_stderr([]() { const haze::TraceSpan span("unit.span"); });
    disable_trace();

    // Exactly two trace records: span begin, then span end (ok, no mark_error).
    const std::vector<TraceRecord> records = collect_trace_records(captured);
    REQUIRE(records.size() == 2);

    // Every record carries the exact current correlation id, and each is one
    // line (collect_trace_records only returns single-line trace records).
    for (const TraceRecord &rec : records)
        REQUIRE(rec.cid == 0x5150ULL);

    REQUIRE(records[0].detail == "trace.span.begin: unit.span");
    REQUIRE(records[1].detail == "trace.span.end.ok: unit.span");

    // No stray newlines: two records means exactly two terminators.
    REQUIRE(std::count(captured.begin(), captured.end(), '\n') == 2);
}

TEST_CASE("observability: tracing is off by default and a span emits nothing", "[unit]") {
    const ObservabilityStateGuard restore;
    disable_trace();
    REQUIRE_FALSE(haze::trace_enabled());
    haze::set_correlation_id(0x99ULL);

    const std::string captured =
        capture_stderr([]() { const haze::TraceSpan span("should.not.emit"); });

    REQUIRE(captured.empty());
}

TEST_CASE("observability: the correlation id is thread-local", "[unit]") {
    const ObservabilityStateGuard restore;
    haze::set_correlation_id(0xAAAAULL);

    // A freshly spawned thread starts from the default id, and mutating it on
    // that thread does not disturb this thread's value.
    std::uint64_t child_before = 0xDEADULL;
    std::uint64_t child_after = 0xDEADULL;
    std::thread worker([&]() {
        child_before = haze::current_correlation_id();
        haze::set_correlation_id(0x1234ULL);
        child_after = haze::current_correlation_id();
    });
    worker.join();

    REQUIRE(child_before == 0ULL);
    REQUIRE(child_after == 0x1234ULL);
    REQUIRE(haze::current_correlation_id() == 0xAAAAULL);
}

// ---------------------------------------------------------------------------
// Integration evidence: a real record -> flush -> replay cycle wires a non-zero
// correlation id onto the flush span ([integration], needs the sim).
// ---------------------------------------------------------------------------

TEST_CASE("observability: a flush emits spans across the record->flush->replay path carrying the "
          "epoch's non-zero correlation id",
          "[integration]") {
    const ObservabilityStateGuard restore;
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim, kQ0);

    const std::vector<TraceRecord> records =
        capture_add_flush_traces(q, /*seed_a=*/1, /*seed_b=*/2);

    // The tracing pillar actually emits along the pipeline (guards against the
    // "helpers exist but nothing emits" defect).
    REQUIRE_FALSE(records.empty());

    // The finalize span brackets the flush: a begin on entry and an ok end on
    // success, both from the production TraceSpan("epoch.flush").
    REQUIRE(any_detail_is(records, "trace.span.begin: epoch.flush"));
    REQUIRE(any_detail_is(records, "trace.span.end.ok: epoch.flush"));

    // Every trace record on the flush path shares one and the same NON-ZERO
    // correlation id -- the epoch published its id onto the logging register.
    const std::uint64_t epoch_cid = records.front().cid;
    REQUIRE(epoch_cid != 0ULL);
    for (const TraceRecord &rec : records)
        REQUIRE(rec.cid == epoch_cid);
}

TEST_CASE("observability: a second epoch is stamped with a distinct non-zero correlation id",
          "[integration]") {
    const ObservabilityStateGuard restore;

    const uint64_t q1 = haze::test::setup_integration_compute_config(kRingDim, kQ0);
    const std::vector<TraceRecord> first = capture_add_flush_traces(q1, /*seed_a=*/1, /*seed_b=*/2);
    REQUIRE_FALSE(first.empty());
    const std::uint64_t cid1 = first.front().cid;

    const uint64_t q2 = haze::test::setup_integration_compute_config(kRingDim, kQ0);
    const std::vector<TraceRecord> second =
        capture_add_flush_traces(q2, /*seed_a=*/3, /*seed_b=*/4);
    REQUIRE_FALSE(second.empty());
    const std::uint64_t cid2 = second.front().cid;

    // Ids are monotonic and non-zero, so a distinct epoch is distinctly tagged.
    REQUIRE(cid1 != 0ULL);
    REQUIRE(cid2 != 0ULL);
    REQUIRE(cid2 > cid1);
}

TEST_CASE("observability: with tracing off a full record->flush cycle emits no trace records",
          "[integration]") {
    const ObservabilityStateGuard restore;
    const uint64_t q = haze::test::setup_integration_compute_config(kRingDim, kQ0);
    disable_trace();
    REQUIRE_FALSE(haze::trace_enabled());

    const std::vector<std::vector<uint64_t>> inputs = {
        haze::test::make_residue(q, /*seed=*/5, kRingDim),
        haze::test::make_residue(q, /*seed=*/6, kRingDim),
    };
    const std::vector<void *> d_in = haze::test::allocate_and_h2d_residues(inputs);
    const std::vector<void *> d_dst = haze::test::allocate_dst_residues(/*count=*/1, kBytes);

    hazeError_t add_rc = HAZE_ERROR_INTERNAL;
    hazeError_t tag_rc = HAZE_ERROR_INTERNAL;
    hazeError_t flush_rc = HAZE_ERROR_INTERNAL;
    const std::string captured = capture_stderr([&]() {
        add_rc = hazeAdd(d_dst[0], d_in[0], d_in[1], /*mod_idx=*/0, nullptr);
        tag_rc = hazeTagOutput(d_dst[0]);
        flush_rc = hazeFlush();
    });

    REQUIRE(add_rc == HAZE_SUCCESS);
    REQUIRE(tag_rc == HAZE_SUCCESS);
    REQUIRE(flush_rc == HAZE_SUCCESS);

    haze::test::free_all_residues(d_in);
    haze::test::free_all_residues(d_dst);

    // No trace records at all when the toggle is off -- the default-quiet stderr
    // surface is preserved (no regression for normal operation).
    REQUIRE(collect_trace_records(captured).empty());
    REQUIRE(captured.find("] trace.span.") == std::string::npos);
}
