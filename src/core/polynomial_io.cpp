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
#include "core/polynomial_io.hpp"

#include <cstdint>
#include <niobium/fhetch_api.h>
#include <string_view>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

bool extract_polynomial_values(const fhetch::Polynomial &p, std::string_view /*tag*/,
                               std::vector<uint64_t> &out) {
    // Read the replayed polynomial's integer components directly from its
    // in-memory representation. int_data() throws std::runtime_error if the
    // polynomial is non-integer or invalid; catch every exception so none
    // crosses the noexcept C ABI boundary that the caller
    // (EpochState::do_materialize_locked) sits behind, surfacing a clean
    // `false` (translated to HAZE_ERROR_INTERNAL) instead of a raw throw.
    try {
        out = p.int_data();
    } catch (...) {
        return false;
    }
    return !out.empty();
}

} // namespace haze
