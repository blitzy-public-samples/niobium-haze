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

#include "core/basis_convert.hpp"

#include "common/errors.hpp"
#include "core/config.hpp"
#include "core/device.hpp"
#include "core/epoch.hpp"
#include "core/metrics.hpp"
#include "core/mrp_polymap.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <haze/haze_types.h>
#include <niobium/fhetch_api.h>
#include <unordered_set>
#include <vector>

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// FBC mode from engine config: reduced_noise selects the centered variant,
// montgomery selects the 4-op (hardware SwitchModulus) center shape.
fhetch::FbcVariant fbc_variant() noexcept {
    return config().reduced_noise() ? fhetch::FbcVariant::ReducedNoise
                                    : fhetch::FbcVariant::Standard;
}

fhetch::FbcCenterShape fbc_center_shape() noexcept {
    return config().montgomery() ? fhetch::FbcCenterShape::FourOp : fhetch::FbcCenterShape::ThreeOp;
}

// Validation helpers; each returns InvalidArgument with a debug-log
// breadcrumb on the first failure, keeping the C ABI shim thin.

std::expected<void, HazeInternalError> validate(const hazeBasisConvertParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.dst_base == nullptr ||
        p.dst_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBasisConvert: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // P7-ABI-01: reject hostile base lengths before build_mrp_locked's reserve()
    // and the ModuliBase pointer arithmetic below. Every RNS base is bounded by
    // the device modulus envelope; an unbounded length would throw
    // std::length_error (aborting across the C ABI) or overflow a pointer.
    if (p.src_base_len > static_cast<size_t>(kMaxCiphertextModuli) ||
        p.dst_base_len > static_cast<size_t>(kMaxCiphertextModuli)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeBasisConvert: base length exceeds supported maximum");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

std::expected<void, HazeInternalError> validate(const hazeModDownParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.rescale_base == nullptr ||
        p.rescale_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // P7-ABI-01: reject hostile base lengths before the src_set construction
    // below (std::unordered_set(src_base, src_base + src_base_len) overflows the
    // iterator range on an unbounded src_base_len) and before build_mrp_locked's
    // reserve(). Every RNS base is bounded by the device modulus envelope.
    if (p.src_base_len > static_cast<size_t>(kMaxCiphertextModuli) ||
        p.rescale_base_len > static_cast<size_t>(kMaxCiphertextModuli)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: base length exceeds supported maximum");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // rescale_base must be a *proper* subset of src_base — equal-length
    // would leave dst empty (upstream fhetch asserts the same).
    if (p.rescale_base_len >= p.src_base_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModDown: rescale_base_len >= src_base_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Foreign-modulus check: every prime in rescale_base must appear in
    // src_base. Doing this HAZE-side rejects bad calls before fhetch's
    // assert (which strips in release).
    std::unordered_set<uint64_t> src_set(p.src_base, p.src_base + p.src_base_len);
    for (size_t j = 0; j < p.rescale_base_len; ++j) {
        if (!src_set.contains(p.rescale_base[j])) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeModDown: rescale_base not subset of src_base");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
    }
    return {};
}

std::expected<void, HazeInternalError> validate(const hazeModUpParams &p) noexcept {
    if (p.src_base == nullptr || p.src_base_len == 0 || p.digit_bases == nullptr ||
        p.digit_base_lens == nullptr || p.digit_count == 0 || p.p_base == nullptr ||
        p.p_base_len == 0) {
        record_internal_error(HazeInternalError::InvalidArgument, "hazeModUp: empty or null base");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // P7-ABI-01: reject hostile counts before the digit_base_lens[i] loop below
    // (an unbounded digit_count reads past the array — the ASan stack-buffer
    // overflow) and before mod_up's reserve()/pointer slicing. Every RNS base
    // and the digit count are bounded by the device modulus envelope.
    if (p.src_base_len > static_cast<size_t>(kMaxCiphertextModuli) ||
        p.p_base_len > static_cast<size_t>(kMaxCiphertextModuli) ||
        p.digit_count > static_cast<size_t>(kMaxCiphertextModuli)) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModUp: base length or digit count exceeds supported maximum");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // digit_bases is a flat concatenation; the per-digit lengths must sum
    // to digit_bases_total_len. Catch caller miscounts before slicing
    // out-of-bounds. Each per-digit length is likewise bounded by the device
    // modulus envelope, so the running sum cannot overflow (P7-ABI-01) and the
    // later emplace_back(digit_bases + offset, + dlen) never overflows a pointer.
    size_t sum = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        if (p.digit_base_lens[i] > static_cast<size_t>(kMaxCiphertextModuli)) {
            record_internal_error(HazeInternalError::InvalidArgument,
                                  "hazeModUp: per-digit base length exceeds supported maximum");
            return std::unexpected(HazeInternalError::InvalidArgument);
        }
        sum += p.digit_base_lens[i];
    }
    if (sum != p.digit_bases_total_len) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "hazeModUp: digit_base_lens do not sum to digit_bases_total_len");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    return {};
}

} // namespace

std::expected<void, HazeInternalError> basis_convert(void *const *dst, const void *const *src,
                                                     const hazeBasisConvertParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    const fhetch::ModuliBase target_base(p.dst_base, p.dst_base + p.dst_base_len);
    // Thread the engine's configured FBC variant/shape, matching mod_down/mod_up
    // below — basis_convert was the lone CRT primitive pinned to the 2-arg
    // fast_base_convert default (ReducedNoise/ThreeOp). With reduced_noise off this
    // yields the Standard lift that a split rescale/keyswitch mod-down (INTT only the
    // dropped limbs, then basis-convert + NTT + combine in eval) composes against
    // byte-for-byte; with reduced_noise on it tracks the centered variant automatically.
    fhetch::MRP result =
        fhetch::fast_base_convert(*src_mrp, target_base, fbc_variant(), fbc_center_shape());
    auto stored = store_mrp_locked(dst, result, p.dst_base, p.dst_base_len);
    if (!stored)
        return stored;
    // One high-level basis-convert op emitted, counted once regardless of
    // residue fan-out; reached only on the success path (M1).
    metrics().add_op();
    return {};
}

std::expected<void, HazeInternalError> mod_down(void *const *dst, const void *const *src,
                                                const hazeModDownParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    const fhetch::ModuliBase rescale_base(p.rescale_base, p.rescale_base + p.rescale_base_len);
    fhetch::MRP result =
        fhetch::rescale_fbc(*src_mrp, rescale_base, fbc_variant(), fbc_center_shape());
    // result.base() == src_base \ rescale_base in src_base's original
    // order. Use it directly so HAZE-side and backend-side never disagree
    // on the dst layout.
    const auto &dst_base = result.base();
    auto stored = store_mrp_locked(dst, result, dst_base.data(), dst_base.size());
    if (!stored)
        return stored;
    // One high-level mod-down op emitted, counted once regardless of residue
    // fan-out; reached only on the success path (M1).
    metrics().add_op();
    return {};
}

std::expected<void, HazeInternalError> mod_up(void *const *dst, const void *const *src,
                                              const hazeModUpParams &p) noexcept {
    if (auto v = validate(p); !v) {
        return v;
    }

    EpochSession session;

    auto src_mrp = build_mrp_locked(src, p.src_base, p.src_base_len);
    if (!src_mrp) {
        return std::unexpected(src_mrp.error());
    }

    std::vector<fhetch::ModuliBase> digit_bases;
    digit_bases.reserve(p.digit_count);
    size_t offset = 0;
    for (size_t i = 0; i < p.digit_count; ++i) {
        const size_t dlen = p.digit_base_lens[i];
        digit_bases.emplace_back(p.digit_bases + offset, p.digit_bases + offset + dlen);
        offset += dlen;
    }

    const fhetch::ModuliBase p_base(p.p_base, p.p_base + p.p_base_len);

    // Open-code fhetch::dig_decomp so the per-digit lift follows the configured FBC
    // variant. dig_decomp hardcodes FbcVariant::ReducedNoise (centered), but mod_down and
    // basis_convert thread config()'s fbc_variant(); mod_up must too, otherwise reduced_noise
    // cannot toggle the keyswitch mod-up and a non-reduced-noise context gets a mismatched
    // (always-centered) digit decomposition. The per-digit target is src_base ∪ p_base (Q∥P),
    // exactly as dig_decomp builds it, and each lifted digit's base equals that target.
    const fhetch::MRP &x = *src_mrp;
    fhetch::ModuliBase target_base = x.base();
    target_base.insert(target_base.end(), p_base.begin(), p_base.end());
    for (size_t d = 0; d < p.digit_count; ++d) {
        const fhetch::MRP digit = fhetch::fast_base_convert(
            fhetch::mr_subset(x, digit_bases[d]), target_base, fbc_variant(), fbc_center_shape());
        const auto &d_base = digit.base();
        auto stored =
            store_mrp_locked(dst + (d * d_base.size()), digit, d_base.data(), d_base.size());
        if (!stored)
            return stored;
    }
    // One high-level mod-up op emitted per API call, counted once regardless of
    // digit/residue fan-out; reached only after every digit stored (M1).
    metrics().add_op();
    return {};
}

} // namespace haze
