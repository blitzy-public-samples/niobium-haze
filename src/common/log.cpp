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
#include "common/log.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace haze {

namespace {
thread_local std::uint64_t g_correlation_id = 0;

// Appends `field` to `out` with every byte that could break single-line log
// framing or emit a terminal control sequence replaced by a printable escape:
// printable ASCII (0x20-0x7E) is copied as-is, a backslash is doubled, and any
// other byte (C0/C1 controls, DEL, and non-ASCII bytes) becomes "\xNN".
void append_escaped(std::string &out, std::string_view field) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (char raw : field) {
        const auto ch = static_cast<unsigned char>(raw);
        if (raw == '\\') {
            out += "\\\\";
        } else if (ch >= 0x20 && ch <= 0x7E) {
            out += raw;
        } else {
            out += "\\x";
            out += kHexDigits[(ch >> 4) & 0x0F];
            out += kHexDigits[ch & 0x0F];
        }
    }
}

// Redact the directory portion of absolute filesystem paths in `field`, keeping
// only the final path component. Transient FHE material (serialized probes,
// program directories) lives under process-chosen temp directories; emitting
// the full path would leak that on-disk location into diagnostics, so the
// leading directories are replaced with a fixed marker while the trailing name
// is preserved for triage. A path token is a run that begins with '/' at a
// delimiter boundary and continues over non-space, non-control bytes; it is
// redacted only when it carries a directory separator beyond the leading '/'
// (so bare roots like "/tmp" and non-path text such as "http://host/x" are left
// untouched).
std::string redact_paths(std::string_view field) {
    std::string out;
    out.reserve(field.size());
    const std::size_t n = field.size();
    std::size_t i = 0;
    while (i < n) {
        const bool boundary = i == 0 || field[i - 1] == ' ' || field[i - 1] == '\t' ||
                              field[i - 1] == '\'' || field[i - 1] == '"' || field[i - 1] == '(' ||
                              field[i - 1] == '=';
        if (field[i] == '/' && boundary) {
            std::size_t j = i;
            std::size_t last_slash = i;
            std::size_t slash_count = 0;
            while (j < n) {
                const auto c = static_cast<unsigned char>(field[j]);
                if (c <= 0x20 || c == 0x7F) // stop at space, C0 control, or DEL
                    break;
                if (field[j] == '/') {
                    last_slash = j;
                    ++slash_count;
                }
                ++j;
            }
            if (slash_count >= 2) {
                out += "<redacted>/";
                out.append(field.substr(last_slash + 1, j - last_slash - 1));
                i = j;
                continue;
            }
        }
        out += field[i];
        ++i;
    }
    return out;
}
} // namespace

std::uint64_t current_correlation_id() noexcept {
    return g_correlation_id;
}

void set_correlation_id(std::uint64_t id) noexcept {
    g_correlation_id = id;
}

CorrelationScope::CorrelationScope(std::uint64_t id) noexcept : previous_(g_correlation_id) {
    g_correlation_id = id;
}

CorrelationScope::~CorrelationScope() {
    g_correlation_id = previous_;
}

void log_error(std::string_view tag, std::string_view body, std::uint64_t correlation_id) noexcept {
    // Compose the whole record first, then emit it with a single fwrite so each
    // event reaches stderr as one atomic, non-interleaved line. The try/catch
    // keeps any allocation or sink failure from escaping this noexcept sink.
    try {
        std::string record = "[haze] [cid=";
        record += std::to_string(correlation_id);
        record += "] ";
        append_escaped(record, tag);
        record += ": ";
        // Redact on-disk locations from the (caller-composed) body before
        // escaping, so a temp/program directory can never leak into a log line.
        // Tags are fixed literals at the call sites and carry no such data.
        append_escaped(record, redact_paths(body));
        record += '\n';
        std::fwrite(record.data(), 1, record.size(), stderr);
    } catch (...) {
        // Last resort if composing or writing the record fails (e.g. bad_alloc):
        // emit a fixed, allocation-free notice so a dropped diagnostic stays
        // visible, and never propagate the failure across the noexcept C ABI.
        std::fputs("[haze] log sink dropped a record (formatting or write failure)\n", stderr);
    }
}

void log_error(std::string_view tag, std::string_view body) noexcept {
    log_error(tag, body, g_correlation_id);
}

std::uint64_t next_correlation_id() noexcept {
    // Monotonic process-wide counter starting at 1, so a generated id is never
    // the 0 "unset" sentinel. Relaxed ordering is sufficient: ids only need to
    // be distinct, not ordered against other memory.
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

bool trace_enabled() noexcept {
    // Re-read on each call (unlike the cached HAZE_DEBUG gate) so tracing can be
    // toggled at runtime and exercised by tests. Any non-empty value other than
    // "0" enables span tracing. Spans are coarse (one pair per flush / graph
    // launch), so a getenv per span is negligible.
    const char *v = std::getenv("HAZE_TRACE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

TraceSpan::TraceSpan(std::string_view name) noexcept : name_(name) {
    if (trace_enabled())
        log_error("trace.span.begin", name_);
}

void TraceSpan::mark_error() noexcept {
    ok_ = false;
}

TraceSpan::~TraceSpan() {
    if (trace_enabled())
        log_error(ok_ ? "trace.span.end.ok" : "trace.span.end.error", name_);
}

} // namespace haze
