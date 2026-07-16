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
#include <niobium/fhetch_api.h>
#include <string_view>
#include <vector>

namespace haze {

// Read the integer-component values out of a fhetch::Polynomial.
//
// The values are read directly from the polynomial's in-memory
// representation via fhetch::Polynomial::int_data(). No temporary file
// or serialization round-trip is involved, so this is safe to call at
// high frequency and from multiple concurrent processes sharing one
// system temp directory (it touches no shared filesystem state).
//
// `tag` is unused; it is retained only for source/ABI stability with the
// existing call sites. Returns true and populates `out` with the integer
// components on success; false if the polynomial is non-integer, invalid,
// or otherwise yields no values (int_data() throws in those cases, which
// is caught so no exception crosses the noexcept C ABI boundary).
bool extract_polynomial_values(const niobium::fhetch::Polynomial &p, std::string_view tag,
                               std::vector<uint64_t> &out);

} // namespace haze
