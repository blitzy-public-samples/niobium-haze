// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Smoke coverage for the three intentional no-op entry points that must retain
// their behaviour: hazeDeviceSynchronize, hazeStreamSynchronize, and
// hazeStreamWaitEvent. Each returns HAZE_SUCCESS without performing work, on
// both an explicit and the default (null) stream, and leaves the last-error
// register clean.
//
// The telemetry tie-in case is the observability evidence for the no-ops:
// because these functions perform no work they must be telemetry- and
// log-neutral. It snapshots the public performance counters and the
// thread-local log correlation id immediately before and after invoking all
// three no-ops (with the handles created outside the snapshot window so the
// no-ops are the only operations between the two reads) and asserts every field
// is unchanged. This locks in the documented behaviour without modifying it.
//
// The two log-integrity cases provide the deterministic executable evidence for
// the structured-logging surface: they capture stderr around haze::log_error
// and assert (1) that one call maps to exactly one line with every
// framing-breaking byte escaped as "\xNN", and (2) that the correlation id
// stamped onto the line is exactly the current thread-local value maintained by
// CorrelationScope. The mechanism rationale is recorded in decision log D-16.

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
#include <unistd.h>
#include <vector>

namespace {

// Frees a stream on scope exit so a mid-case REQUIRE failure cannot leak it.
class StreamGuard {
  public:
    explicit StreamGuard(hazeStream_t s) noexcept : s_(s) {}
    StreamGuard(const StreamGuard &) = delete;
    StreamGuard &operator=(const StreamGuard &) = delete;
    StreamGuard(StreamGuard &&) = delete;
    StreamGuard &operator=(StreamGuard &&) = delete;
    ~StreamGuard() {
        if (s_ != nullptr)
            (void)hazeStreamDestroy(s_);
    }
    hazeStream_t get() const noexcept { return s_; }

  private:
    hazeStream_t s_ = nullptr;
};

// Frees an event on scope exit for the same reason.
class EventGuard {
  public:
    explicit EventGuard(hazeEvent_t e) noexcept : e_(e) {}
    EventGuard(const EventGuard &) = delete;
    EventGuard &operator=(const EventGuard &) = delete;
    EventGuard(EventGuard &&) = delete;
    EventGuard &operator=(EventGuard &&) = delete;
    ~EventGuard() {
        if (e_ != nullptr)
            (void)hazeEventDestroy(e_);
    }
    hazeEvent_t get() const noexcept { return e_; }

  private:
    hazeEvent_t e_ = nullptr;
};

// Runs `emit` with stderr redirected to an in-memory temp file and returns the
// bytes it wrote. The real stderr is restored before returning so the test
// framework's own diagnostics are unaffected; assertions run on the returned
// string after restoration. Best-effort: if capture cannot be set up, `emit`
// still runs and an empty string is returned (which the callers assert against).
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

    // Read the captured bytes back with checked stdio calls (fseek/ftell/one
    // fread) so no stream-position or error state is left unhandled.
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

} // namespace

TEST_CASE("documented no-op: hazeDeviceSynchronize returns success", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamSynchronize returns success on the default stream",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(nullptr) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamSynchronize returns success on a real stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    StreamGuard stream_guard(s);
    REQUIRE(hazeStreamSynchronize(s) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamWaitEvent returns success", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    StreamGuard stream_guard(s);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    EventGuard event_guard(e);
    REQUIRE(hazeStreamWaitEvent(s, e, 0U) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: hazeStreamWaitEvent returns success on the default stream", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    EventGuard event_guard(e);
    REQUIRE(hazeStreamWaitEvent(nullptr, e, 0U) == HAZE_SUCCESS);
}

TEST_CASE("documented no-op: no-op calls leave the last-error register clean", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(nullptr) == HAZE_SUCCESS);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    EventGuard event_guard(e);
    REQUIRE(hazeStreamWaitEvent(nullptr, e, 0U) == HAZE_SUCCESS);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE(
    "documented no-op: the no-ops perturb neither the metrics counters nor the correlation id",
    "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // Handles are created before the snapshot window so that the only
    // operations between the two counter reads are the three no-ops.
    hazeStream_t s = nullptr;
    REQUIRE(hazeStreamCreate(&s) == HAZE_SUCCESS);
    StreamGuard stream_guard(s);
    hazeEvent_t e = nullptr;
    REQUIRE(hazeEventCreate(&e) == HAZE_SUCCESS);
    EventGuard event_guard(e);

    // A non-default correlation context makes a spurious reset observable.
    const haze::CorrelationScope scope(0xC0FFEEULL);
    REQUIRE(haze::current_correlation_id() == 0xC0FFEEULL);

    hazePerformanceCounters before{};
    REQUIRE(hazeGetPerformanceCounters(&before) == HAZE_SUCCESS);
    const std::uint64_t cid_before = haze::current_correlation_id();

    REQUIRE(hazeDeviceSynchronize() == HAZE_SUCCESS);
    REQUIRE(hazeStreamSynchronize(s) == HAZE_SUCCESS);
    REQUIRE(hazeStreamWaitEvent(s, e, 0U) == HAZE_SUCCESS);

    hazePerformanceCounters after{};
    REQUIRE(hazeGetPerformanceCounters(&after) == HAZE_SUCCESS);
    const std::uint64_t cid_after = haze::current_correlation_id();

    // No op emitted, no byte moved, no flush recorded, correlation unchanged.
    REQUIRE(after.op_count == before.op_count);
    REQUIRE(after.bytes_h2d == before.bytes_h2d);
    REQUIRE(after.bytes_d2h == before.bytes_d2h);
    REQUIRE(after.bytes_d2d == before.bytes_d2d);
    REQUIRE(after.flush_count == before.flush_count);
    REQUIRE(after.flush_time_ns_total == before.flush_time_ns_total);
    REQUIRE(after.flush_time_ns_last == before.flush_time_ns_last);
    REQUIRE(cid_after == cid_before);
}

TEST_CASE("observability evidence: log_error emits exactly one escaped line stamped with the id",
          "[unit]") {
    const std::string tag = "noop-probe";
    // A body that would break single-line framing (and emit terminal control
    // sequences) if it were written raw: LF, CR, tab, DEL, and a high byte.
    std::string body = "line-one";
    body += '\n';
    body += "line-two";
    body += '\r';
    body += "col\tafter-tab";
    body += static_cast<char>(0x7F); // DEL
    body += "mid";
    body += static_cast<char>(0xFF); // non-ASCII
    body += "end";
    const std::uint64_t cid = 0xABCDEF01ULL;

    const std::string captured = capture_stderr([&]() { haze::log_error(tag, body, cid); });

    // Exactly one line: the record terminates with '\n' and contains no other.
    REQUIRE_FALSE(captured.empty());
    REQUIRE(captured.back() == '\n');
    REQUIRE(std::count(captured.begin(), captured.end(), '\n') == 1);

    // None of the framing-breaking raw bytes survived into the emitted record.
    REQUIRE(captured.find('\r') == std::string::npos);
    REQUIRE(captured.find('\t') == std::string::npos);
    REQUIRE(captured.find(static_cast<char>(0x7F)) == std::string::npos);
    REQUIRE(captured.find(static_cast<char>(0xFF)) == std::string::npos);

    // They appear instead as the documented lowercase "\xNN" escapes.
    REQUIRE(captured.find("\\x0a") != std::string::npos); // LF
    REQUIRE(captured.find("\\x0d") != std::string::npos); // CR
    REQUIRE(captured.find("\\x09") != std::string::npos); // tab
    REQUIRE(captured.find("\\x7f") != std::string::npos); // DEL
    REQUIRE(captured.find("\\xff") != std::string::npos); // high byte

    // Deterministic prefix: fixed marker, decimal correlation id, then the tag.
    const std::string expected_prefix = "[haze] [cid=" + std::to_string(cid) + "] " + tag + ": ";
    REQUIRE(captured.rfind(expected_prefix, 0) == 0);
}

TEST_CASE("observability evidence: correlation scope is deterministic and stamps the log line",
          "[unit]") {
    // Start from a known baseline so nesting is fully deterministic.
    haze::set_correlation_id(0ULL);
    REQUIRE(haze::current_correlation_id() == 0ULL);

    {
        const haze::CorrelationScope outer(0x1111ULL);
        REQUIRE(haze::current_correlation_id() == 0x1111ULL);
        {
            const haze::CorrelationScope inner(0x2222ULL);
            REQUIRE(haze::current_correlation_id() == 0x2222ULL);

            // The 2-arg log_error stamps the *current* thread-local id, so this
            // proves the scope value flows onto the emitted record.
            const std::string captured =
                capture_stderr([&]() { haze::log_error("scope", "body"); });
            const std::string want = "[haze] [cid=" + std::to_string(0x2222ULL) + "] scope: body\n";
            REQUIRE(captured == want);
        }
        // Inner scope restored the outer id exactly.
        REQUIRE(haze::current_correlation_id() == 0x1111ULL);
    }
    // Outer scope restored the baseline exactly.
    REQUIRE(haze::current_correlation_id() == 0ULL);
}

TEST_CASE("observability evidence: next_correlation_id yields distinct non-zero ids", "[unit]") {
    // The generator must never return the 0 "unset" sentinel and must advance,
    // so ids from different record->flush->replay cycles never collide.
    const std::uint64_t a = haze::next_correlation_id();
    const std::uint64_t b = haze::next_correlation_id();
    const std::uint64_t c = haze::next_correlation_id();
    REQUIRE(a != 0ULL);
    REQUIRE(b != 0ULL);
    REQUIRE(c != 0ULL);
    REQUIRE(b > a);
    REQUIRE(c > b);
}

TEST_CASE("observability evidence: log sink redacts on-disk paths, keeping the basename",
          "[unit]") {
    // A body carrying an absolute path to transient FHE material: the directory
    // prefix must be redacted so the on-disk location cannot leak, while the
    // trailing name is kept for triage.
    const std::string sensitive =
        "load failed at /tmp/haze-abc123/serialized_probes/haze_out_0.ct now";
    const std::string captured =
        capture_stderr([&]() { haze::log_error("probe", sensitive, 0x2A2AULL); });

    REQUIRE(captured.find("<redacted>/haze_out_0.ct") != std::string::npos);
    REQUIRE(captured.find("/tmp/haze-abc123") == std::string::npos);
    REQUIRE(captured.find("serialized_probes") == std::string::npos);

    // A body with no absolute path is emitted verbatim (after escaping): the
    // redactor must not perturb ordinary diagnostics.
    const std::string plain = capture_stderr([&]() { haze::log_error("t", "no path here", 7ULL); });
    REQUIRE(plain == "[haze] [cid=7] t: no path here\n");

    // A URL is not a filesystem path token (its slashes sit after ':' or other
    // slashes, never at a redaction boundary), so it survives intact.
    const std::string url =
        capture_stderr([&]() { haze::log_error("t", "see http://host/a/b end", 7ULL); });
    REQUIRE(url.find("http://host/a/b") != std::string::npos);
}

TEST_CASE("observability evidence: TraceSpan emits begin/end only when HAZE_TRACE is enabled",
          "[unit]") {
    const std::uint64_t cid = 0x5150ULL;
    const std::string begin =
        "[haze] [cid=" + std::to_string(cid) + "] trace.span.begin: unit.op\n";
    const std::string end_err =
        "[haze] [cid=" + std::to_string(cid) + "] trace.span.end.error: unit.op\n";

    // Enabled: begin on construction, end.error on destruction (mark_error).
    setenv("HAZE_TRACE", "1", 1); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
    const std::string on = capture_stderr([&]() {
        const haze::CorrelationScope scope(cid);
        haze::TraceSpan span("unit.op");
        span.mark_error();
    });
    unsetenv("HAZE_TRACE"); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
    REQUIRE(on.find(begin) != std::string::npos);
    REQUIRE(on.find(end_err) != std::string::npos);

    // Disabled: the span is inert and emits nothing.
    const std::string off = capture_stderr([&]() {
        const haze::CorrelationScope scope(cid);
        haze::TraceSpan span("unit.op");
    });
    REQUIRE(off.empty());
}

TEST_CASE("observability evidence: readiness reflects the post-reset lifecycle state", "[unit]") {
    // After a full device reset nothing is configured and no epoch is recording,
    // so the readiness query -- backed by real config/epoch state, not a
    // hard-coded value -- reports both false.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const haze::RuntimeReadiness ready = haze::runtime_readiness();
    REQUIRE_FALSE(ready.configured);
    REQUIRE_FALSE(ready.epoch_active);
}

TEST_CASE("observability evidence: a production flush generates a real correlation id", "[unit]") {
    // Even an idle flush routes through the production replay_and_populate path,
    // which opens a CorrelationScope(next_correlation_id()); observing the
    // global generator advance across the call proves the flush path stamps a
    // non-zero id (the O1 gap was that production ids stayed 0).
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const std::uint64_t before = haze::next_correlation_id();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    const std::uint64_t after = haze::next_correlation_id();
    // `before` and `after` are my two probes; the flush consumed >= 1 id between
    // them, so the gap is at least 2.
    REQUIRE(after >= before + 2ULL);
}

TEST_CASE("observability evidence: readiness and correlation track a real record/flush cycle",
          "[integration]") {
    // End-to-end proof that the observability surfaces reflect production flow,
    // not documentation: readiness follows configure -> record -> flush, and a
    // real flush stamps a fresh correlation id.
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    {
        const haze::RuntimeReadiness before = haze::runtime_readiness();
        REQUIRE_FALSE(before.configured);
        REQUIRE_FALSE(before.epoch_active);
    }

    (void)haze::test::setup_integration_compute_config(4096, 576460752303415297ULL, /*mod_idx=*/0);
    {
        const haze::RuntimeReadiness configured = haze::runtime_readiness();
        REQUIRE(configured.configured);
    }

    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    void *dst = nullptr;
    void *src1 = nullptr;
    void *src2 = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&src1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&src2, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> host(4096, 1ULL);
    REQUIRE(hazeMemcpy(src1, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src2, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, src1, src2, /*mod_idx=*/0, nullptr) == HAZE_SUCCESS);

    {
        // A recording is now open: configured, backend initialized, epoch active.
        const haze::RuntimeReadiness recording = haze::runtime_readiness();
        REQUIRE(recording.configured);
        REQUIRE(recording.backend_initialized);
        REQUIRE(recording.epoch_active);
    }

    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    const std::uint64_t before = haze::next_correlation_id();
    REQUIRE(hazeFlush() == HAZE_SUCCESS);
    const std::uint64_t after = haze::next_correlation_id();
    REQUIRE(after >= before + 2ULL);

    {
        // The flush drained the epoch, so no recording remains active.
        const haze::RuntimeReadiness drained = haze::runtime_readiness();
        REQUIRE_FALSE(drained.epoch_active);
    }

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src2) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("observability evidence: a real record/flush cycle emits a trace span carrying one "
          "non-zero correlation id",
          "[integration]") {
    // The strongest end-to-end propagation proof: drive a real recorded op and
    // flush with span tracing enabled, capture everything the production path
    // writes to the diagnostic sink, and confirm the flush emitted its
    // "epoch.flush" span pair stamped with a single non-zero correlation id.
    // The cases above observe the id *generator* advancing; this one observes
    // the id actually reaching an emitted record -- the O1 gap was that
    // production diagnostics carried id 0, which the generator-only checks
    // cannot detect.
    (void)haze::test::setup_integration_compute_config(4096, 576460752303415297ULL, /*mod_idx=*/0);

    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    void *dst = nullptr;
    void *src1 = nullptr;
    void *src2 = nullptr;
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&src1, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&src2, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> host(4096, 1ULL);
    REQUIRE(hazeMemcpy(src1, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(src2, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, src1, src2, /*mod_idx=*/0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);

    // Enable span tracing (trace_enabled() re-reads the environment on each
    // call, so this takes effect immediately for the flush below), capture the
    // sink across a real flush, then restore the environment before any
    // assertion can throw.
    setenv("HAZE_TRACE", "1", 1); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
    hazeError_t flush_rc = HAZE_ERROR_INTERNAL;
    const std::string captured = capture_stderr([&]() { flush_rc = hazeFlush(); });
    unsetenv("HAZE_TRACE"); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>

    REQUIRE(flush_rc == HAZE_SUCCESS);

    // The production flush path opened a TraceSpan("epoch.flush"): with tracing
    // on it emitted a begin record on entry and an ok end record on success.
    const std::string begin_tag = "trace.span.begin: epoch.flush";
    const std::string end_tag = "trace.span.end.ok: epoch.flush";
    const std::size_t begin_pos = captured.find(begin_tag);
    const std::size_t end_pos = captured.find(end_tag);
    REQUIRE(begin_pos != std::string::npos);
    REQUIRE(end_pos != std::string::npos);

    // Extract the correlation id stamped on each span record: the "[cid=<N>]"
    // field immediately precedes the tag on the same single-line record.
    const auto cid_before = [&captured](std::size_t tag_pos) -> std::uint64_t {
        const std::string key = "[cid=";
        const std::size_t k = captured.rfind(key, tag_pos);
        REQUIRE(k != std::string::npos);
        std::uint64_t value = 0;
        for (std::size_t i = k + key.size();
             i < captured.size() && captured[i] >= '0' && captured[i] <= '9'; ++i)
            value = value * 10U + static_cast<std::uint64_t>(captured[i] - '0');
        return value;
    };
    const std::uint64_t begin_cid = cid_before(begin_pos);
    const std::uint64_t end_cid = cid_before(end_pos);

    // A real, non-zero id was installed by the flush (not the 0 "unset"
    // sentinel), and the begin/end pair share it, proving one correlation
    // context framed the whole record->flush->replay cycle.
    REQUIRE(begin_cid != 0ULL);
    REQUIRE(begin_cid == end_cid);

    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src1) == HAZE_SUCCESS);
    REQUIRE(hazeFree(src2) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
