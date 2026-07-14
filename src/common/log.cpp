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

#include <cstdint>
#include <cstdio>
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
        append_escaped(record, body);
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

} // namespace haze
