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

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string>
#include <unistd.h>

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
