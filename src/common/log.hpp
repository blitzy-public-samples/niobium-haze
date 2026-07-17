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
#pragma once

#include <cstdint>
#include <string_view>

namespace haze {

// Tagged sink for [haze]-prefixed runtime diagnostics emitted from
// replay_bridge (libhaze internal failures route through
// record_internal_error in errors.hpp instead).
//
// Output format: "[haze] [cid=<id>] <tag>: <body>\n", where <id> is the
// current thread-local correlation id (0 when unset). Callers compose their
// own error text; <tag> and <body> are emitted as a single record with
// control, DEL, and non-ASCII bytes escaped as "\xNN", so one call always
// maps to exactly one line.
void log_error(std::string_view tag, std::string_view body) noexcept;

// As above, but stamps the line with an explicit correlation id instead of
// the current thread-local value.
void log_error(std::string_view tag, std::string_view body, std::uint64_t correlation_id) noexcept;

// Thread-local correlation id stamped onto subsequent log_error() lines.
// Defaults to 0 ("none"). Intended to be set/scoped by the epoch/stream
// layers so diagnostics from one record->flush->replay cycle correlate.
std::uint64_t current_correlation_id() noexcept;
void set_correlation_id(std::uint64_t id) noexcept;

// Sets the thread-local correlation id for the lifetime of the object and
// restores the previous value on destruction.
class CorrelationScope {
  public:
    explicit CorrelationScope(std::uint64_t id) noexcept;
    ~CorrelationScope();
    CorrelationScope(const CorrelationScope &) = delete;
    CorrelationScope &operator=(const CorrelationScope &) = delete;

  private:
    std::uint64_t previous_;
};

// Returns a fresh, process-unique correlation id (monotonic, never 0). The
// runtime calls this once per record->flush->replay cycle (flush and graph
// launch) and installs it via CorrelationScope, so every diagnostic emitted
// during that cycle -- including those from the replay_bridge on the same
// thread -- shares one id and can be correlated after the fact.
std::uint64_t next_correlation_id() noexcept;

// True when span tracing is enabled via the HAZE_TRACE environment variable
// (any non-empty value other than "0"), re-read on each call so tracing can be
// toggled at runtime (and exercised by tests). When false, TraceSpan begin/end
// records are suppressed; error diagnostics still flow through log_error
// unconditionally.
bool trace_enabled() noexcept;

// RAII span bracketing one traced runtime operation. When trace_enabled(), the
// constructor emits a "trace: begin <name> ..." record and the destructor emits
// a matching "trace: end <name> status=ok|error" record, both stamped with the
// current correlation id, so a span's begin/end pair frames all diagnostics
// emitted between them. When tracing is disabled the span is inert. `name` must
// outlive the span (callers pass string literals); the type is move/copy-free.
class TraceSpan {
  public:
    explicit TraceSpan(std::string_view name) noexcept;
    ~TraceSpan();
    // Flip the span's terminal status to "error" (reported by the destructor).
    void mark_error() noexcept;
    TraceSpan(const TraceSpan &) = delete;
    TraceSpan &operator=(const TraceSpan &) = delete;

  private:
    std::string_view name_;
    bool ok_ = true;
};

// Readiness snapshot: the library-context reinterpretation of a service health
// probe (see docs/observability/README.md). Each field answers one lifecycle /
// configuration-state question a linked application can poll before driving the
// runtime. Definition lives in the core runtime (epoch.cpp) so this common-layer
// header stays free of core dependencies.
struct RuntimeReadiness {
    bool configured;          // a crypto context / ring dimension is configured
    bool backend_initialized; // the compiler backend has initialized successfully
    bool epoch_active;        // an epoch is currently recording
};

// Returns the current readiness snapshot. Each field is read under its own
// subsystem lock, acquired and released independently (no lock is held while
// another is taken), so this adds no new lock-ordering edge.
RuntimeReadiness runtime_readiness() noexcept;

} // namespace haze
