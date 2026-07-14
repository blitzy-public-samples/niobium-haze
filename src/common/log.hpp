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
// current thread-local correlation id (0 when unset). Both tag and body are
// printed verbatim; callers compose their own error text.
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

} // namespace haze
