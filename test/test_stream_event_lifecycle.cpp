// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
//
// Backfill coverage for the stream and event lifecycle surface: create,
// create-with-priority / create-with-flags, destroy, and event record. These
// entry points model CUDA-shape handles; the runtime stores an identity id per
// handle and does not model ordering, so record is accepted unconditionally.
//
// The create-with-priority / create-with-flags variants accept and discard
// their flags/priority arguments for CUDA-shape parity: any flags value (zero
// or nonzero) and any priority yields a usable handle. Both variants still
// reject a null output pointer. Destroy and record tolerate a null handle as a
// well-defined no-op. The handles are backed by raw new/delete with no
// registry, so reuse of a destroyed handle is undefined behaviour and is
// deliberately not exercised here.

#include <catch2/catch_test_macros.hpp>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep

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

} // namespace

// ---------------------------------------------------------------------------
// Stream create / destroy ([unit]).
// ---------------------------------------------------------------------------

TEST_CASE("stream lifecycle: create then destroy round-trips", "[unit]") {
    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    REQUIRE(stream != nullptr);
    StreamGuard guard(stream);
}

TEST_CASE("stream lifecycle: create rejects a null output pointer", "[unit]") {
    REQUIRE(hazeStreamCreate(nullptr) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("stream lifecycle: create-with-priority rejects a null output pointer", "[unit]") {
    // The priority variant validates its output pointer exactly like the base
    // creator before touching the ignored flags / priority arguments.
    REQUIRE(hazeStreamCreateWithPriority(nullptr, 0U, 0) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("stream lifecycle: create-with-priority accepts nonzero flags and any priority",
          "[unit]") {
    // Nonzero flags together with a nonzero priority must still yield a usable
    // stream: the arguments are accepted for CUDA-shape parity and discarded.
    hazeStream_t high = nullptr;
    REQUIRE(hazeStreamCreateWithPriority(&high, 1U, -1) == HAZE_SUCCESS);
    REQUIRE(high != nullptr);
    StreamGuard high_guard(high);

    hazeStream_t low = nullptr;
    REQUIRE(hazeStreamCreateWithPriority(&low, 4U, 0) == HAZE_SUCCESS);
    REQUIRE(low != nullptr);
    StreamGuard low_guard(low);

    // A stream produced through the flags path is usable as a record target.
    hazeEvent_t event = nullptr;
    REQUIRE(hazeEventCreate(&event) == HAZE_SUCCESS);
    EventGuard event_guard(event);
    REQUIRE(hazeEventRecord(event, high) == HAZE_SUCCESS);
    REQUIRE(hazeEventRecord(event, low) == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// Event create / destroy / record ([unit]).
// ---------------------------------------------------------------------------

TEST_CASE("event lifecycle: create then destroy round-trips", "[unit]") {
    hazeEvent_t event = nullptr;
    REQUIRE(hazeEventCreate(&event) == HAZE_SUCCESS);
    REQUIRE(event != nullptr);
    EventGuard guard(event);
}

TEST_CASE("event lifecycle: create rejects a null output pointer", "[unit]") {
    REQUIRE(hazeEventCreate(nullptr) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("event lifecycle: create-with-flags rejects a null output pointer", "[unit]") {
    REQUIRE(hazeEventCreateWithFlags(nullptr, 0U) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("event lifecycle: create-with-flags accepts a nonzero flags value", "[unit]") {
    // A nonzero flags value is accepted and discarded, producing a usable event.
    hazeEvent_t event = nullptr;
    REQUIRE(hazeEventCreateWithFlags(&event, 2U) == HAZE_SUCCESS);
    REQUIRE(event != nullptr);
    EventGuard guard(event);

    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);
    REQUIRE(hazeEventRecord(event, stream) == HAZE_SUCCESS);
}

TEST_CASE("event lifecycle: record targets an explicit stream and the default stream", "[unit]") {
    hazeEvent_t event = nullptr;
    REQUIRE(hazeEventCreate(&event) == HAZE_SUCCESS);
    EventGuard event_guard(event);

    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);

    REQUIRE(hazeEventRecord(event, stream) == HAZE_SUCCESS);
    REQUIRE(hazeEventRecord(event, nullptr) == HAZE_SUCCESS); // default stream
}

// ---------------------------------------------------------------------------
// Null-handle tolerance ([unit]).
//
// Destroy and record treat a null handle as a well-defined no-op. Reuse of a
// destroyed (non-null) handle would be undefined behaviour under the raw
// new/delete backing store and is intentionally excluded.
// ---------------------------------------------------------------------------

TEST_CASE("stream lifecycle: destroy tolerates a null handle", "[unit]") {
    REQUIRE(hazeStreamDestroy(nullptr) == HAZE_SUCCESS);
    (void)hazeGetLastError();
}

TEST_CASE("event lifecycle: destroy tolerates a null handle", "[unit]") {
    REQUIRE(hazeEventDestroy(nullptr) == HAZE_SUCCESS);
    (void)hazeGetLastError();
}

TEST_CASE("event lifecycle: record tolerates a null event and a null stream", "[unit]") {
    // A null event, a null stream, and both null together are each accepted.
    REQUIRE(hazeEventRecord(nullptr, nullptr) == HAZE_SUCCESS);

    hazeStream_t stream = nullptr;
    REQUIRE(hazeStreamCreate(&stream) == HAZE_SUCCESS);
    StreamGuard stream_guard(stream);
    REQUIRE(hazeEventRecord(nullptr, stream) == HAZE_SUCCESS);

    hazeEvent_t event = nullptr;
    REQUIRE(hazeEventCreate(&event) == HAZE_SUCCESS);
    EventGuard event_guard(event);
    REQUIRE(hazeEventRecord(event, nullptr) == HAZE_SUCCESS);
    (void)hazeGetLastError();
}
