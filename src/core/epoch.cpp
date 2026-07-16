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
#include "core/epoch.hpp"

#include "common/errors.hpp"
#include "common/handle.hpp"
#include "common/log.hpp"
#include "common/thread_safety.hpp"
#include "core/allocator.hpp"
#include "core/backend.hpp"
#include "core/config.hpp"
#include "core/metrics.hpp"
#include "core/polynomial_io.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <haze/replay_bridge.h>
#include <ios>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

// libnbfhetch internal helpers (declared in the library's private
// compiler_internal.h, which is not on HAZE's include path). They are exported
// from libnbfhetch.a with external linkage, so forward-declaring them here lets
// graph replay repopulate the compiler's captured inputs and map a recorded
// Polynomial back to its synthetic FHETCH address without reaching into the
// opaque PolynomialImpl. Signatures MUST match the library verbatim so the
// mangled names resolve at link time.
namespace niobium::detail {
uintptr_t polynomial_address(const niobium::fhetch::Polynomial &p);
void for_each_captured_input(const std::function<void(const niobium::CapturedInputRecord &)> &cb);
} // namespace niobium::detail

namespace haze {

namespace fhetch = niobium::fhetch;

namespace {

// FNV-1a 64-bit accumulation over a byte range. Used to build a structural
// signature of a recorded op-sequence (see trace_signature_for_dir): folding
// only the semantically-significant lines — with operand register numbers
// canonicalized (see fold_canonical_line) — makes the signature invariant to
// the volatile "# Generated: <timestamp>" header and to the absolute SSA
// numbering of a pure input rebind, while still distinguishing a different
// operation (sr_addp vs sr_mulp) or a different op count.
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnv1a_accumulate(uint64_t hash, std::string_view data) noexcept {
    for (const char c : data) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= kFnvPrime;
    }
    return hash;
}

// Trim ASCII whitespace from both ends of `line` in place.
std::string_view trim_view(std::string_view line) noexcept {
    size_t begin = 0;
    size_t end = line.size();
    while (begin < end && (std::isspace(static_cast<unsigned char>(line[begin])) != 0))
        ++begin;
    while (end > begin && (std::isspace(static_cast<unsigned char>(line[end - 1])) != 0))
        --end;
    return line.substr(begin, end - begin);
}

// Fold a single trimmed trace line into `hash`, rewriting every operand
// register token "%<digits>" to a canonical "%r<first-appearance-index>" using
// `reg_map` (which carries the numbering across the whole trace file). The
// FHETCH trace writer assigns operand ids from allocation history, so a pure
// input rebind (a+b vs a+c) emits the same opcode/dataflow structure but can
// differ in the absolute ids (e.g. "sr_addp %3, %0, %1" vs "sr_addp %2, %0,
// %1"). Canonicalizing by first-appearance order collapses that volatile
// numbering while preserving which operands reuse earlier values, so a rebind
// hashes equal but a different opcode (sr_mulp) or dataflow shape still differs.
// Tokens that are not "%<digits>" (opcodes, "m=1", the "m[0] 0x..." modulus
// table) pass through byte-for-byte, keeping the modulus values topology-
// significant.
uint64_t fold_canonical_line(uint64_t hash, std::string_view line,
                             std::unordered_map<uint64_t, uint64_t> &reg_map) noexcept {
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        if (line[i] == '%' && i + 1 < n &&
            std::isdigit(static_cast<unsigned char>(line[i + 1])) != 0) {
            size_t j = i + 1;
            uint64_t reg = 0;
            while (j < n && std::isdigit(static_cast<unsigned char>(line[j])) != 0) {
                reg = (reg * 10U) + static_cast<uint64_t>(line[j] - '0');
                ++j;
            }
            const auto [it, inserted] = reg_map.try_emplace(reg, reg_map.size());
            hash = fnv1a_accumulate(hash, "%r");
            char buf[20];
            const auto res = std::to_chars(buf, buf + sizeof(buf), it->second);
            if (res.ec == std::errc())
                hash = fnv1a_accumulate(hash,
                                        std::string_view(buf, static_cast<size_t>(res.ptr - buf)));
            i = j;
        } else {
            hash = fnv1a_accumulate(hash, std::string_view(&line[i], 1));
            ++i;
        }
    }
    return hash;
}

// Compute a structural signature of the recorded op-sequence by hashing the
// non-comment, non-blank lines of every .fhetch trace under `dir` (the modulus
// table + the sr_* instruction stream + halt), with operand register numbers
// canonicalized per file (see fold_canonical_line). Comment lines ('#') —
// including the volatile "# Generated" timestamp and the "# output" bindings —
// are excluded, so two captures that differ only in their input operands (a
// pure rebind such as a+b vs a+c) hash equal, while a different operation or op
// count hashes differently. Files are visited in sorted path order for
// determinism, and the register numbering resets per file since each .fhetch is
// an independent program. Returns 0 on any I/O failure or when no trace is
// found (an all-comment/empty digest also folds to the offset basis, distinct
// from 0).
uint64_t trace_signature_for_dir(const std::filesystem::path &dir) noexcept {
    std::vector<std::filesystem::path> traces;
    try {
        if (!std::filesystem::exists(dir))
            return 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".fhetch")
                traces.push_back(entry.path());
        }
    } catch (...) {
        return 0;
    }
    if (traces.empty())
        return 0;
    std::ranges::sort(traces);

    uint64_t hash = kFnvOffsetBasis;
    try {
        for (const auto &trace : traces) {
            std::ifstream in(trace);
            if (!in)
                return 0;
            std::unordered_map<uint64_t, uint64_t> reg_map;
            std::string line;
            while (std::getline(in, line)) {
                const std::string_view trimmed = trim_view(line);
                if (trimmed.empty() || trimmed.front() == '#')
                    continue;
                hash = fold_canonical_line(hash, trimmed, reg_map);
                hash = fnv1a_accumulate(hash, "\n");
            }
        }
    } catch (...) {
        return 0;
    }
    return hash;
}

} // namespace

EpochState &EpochState::instance() noexcept {
    static EpochState inst;
    return inst;
}

void EpochState::ensure_recording_locked() {
    // EpochSession initializes the backend before locking; the
    // is_initialized() check guards future code paths that might bypass
    // EpochSession. start_epoch() before start() memorizes the
    // polynomial-ID base so post-materialize resets snap back to it;
    // without it, IDs drift.
    if (backend().is_initialized() && !recording_) {
        niobium::compiler().start_epoch();
        CompilerBackend::start_recording();
        recording_ = true;
    }
}

void EpochState::evict_mrp_group_locked(const std::string &name) noexcept {
    auto group_it = known_mrp_groups_.find(name);
    if (group_it == known_mrp_groups_.end())
        return;
    for (DevAddr member : group_it->second.addrs) {
        auto o = addr_to_mrp_groups_.find(member);
        if (o == addr_to_mrp_groups_.end())
            continue;
        o->second.erase(name);
        if (o->second.empty())
            addr_to_mrp_groups_.erase(o);
    }
    known_mrp_groups_.erase(group_it);
    pending_mrp_groups_.erase(name);
}

void EpochState::invalidate(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    // Drop any MRP group that names addr so a recycled allocation can't
    // bind a stale group entry to a new polynomial at replay time.
    if (auto rev_it = addr_to_mrp_groups_.find(addr); rev_it != addr_to_mrp_groups_.end()) {
        // Move the names out and erase addr's entry first: evict's per-member
        // sweep edits the reverse map (would invalidate rev_it) and skips addr
        // for free (replaces the old `if (other == addr) continue;` guard).
        auto group_names = std::move(rev_it->second);
        addr_to_mrp_groups_.erase(rev_it);
        for (const auto &name : group_names)
            evict_mrp_group_locked(name);
    }
    // pending_outputs_ is a subset of poly_map_; erase-on-miss is O(1), so
    // unconditional double-erase is simpler than tagging addrs by owner.
    poly_map_.erase(addr);
    pending_outputs_.erase(addr);
    addr_modulus_.erase(addr);
    input_addrs_.erase(addr);
    // A recycled allocation leading a new group gets a fresh name rather
    // than colliding with a possibly-pending group from its previous life.
    mrp_in_names_.erase(addr);
    mrp_out_names_.erase(addr);
}

std::expected<niobium::fhetch::Polynomial, HazeInternalError>
EpochState::lookup_or_create_locked(DevAddr addr) {
    if (auto it = poly_map_.find(addr); it != poly_map_.end()) {
        return it->second;
    }

    const uint64_t ring_dim = config().ring_dim();
    // Graph capture must not consume the caller's input buffers: capturing a
    // graph leaves the user's device allocations intact and reusable (CUDA
    // stream-capture semantics), so a subsequent capture reading the same
    // inputs still resolves. Read non-evictingly while a capture is active;
    // outside capture the evicting read frees the HAZE-side shadow mid-program
    // as before.
    auto components = capturing_ ? allocator().read_polynomial_components(addr, ring_dim)
                                 : allocator().extract_polynomial_components(addr, ring_dim);
    if (!components) {
        // Compute / D2D on an addr with neither shadow data nor a
        // poly_map_ binding is undefined under the record-and-replay
        // model — there's no value to read. Translate NoData into the
        // sharper SourceUnavailable; pass other errors through.
        if (components.error() == HazeInternalError::NoData) {
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "lookup_or_create_locked: no shadow and no poly_map_ binding");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
        return std::unexpected(components.error());
    }
    fhetch::Polynomial poly =
        fhetch::Polynomial::from_data(std::move(*components), ring_dim, fhetch::Format::Evaluation);
    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::tag_input(name, poly);
    poly_map_.emplace(addr, poly);
    input_addrs_.insert(addr);
    return poly;
}

bool EpochState::is_input_locked(DevAddr addr) const noexcept {
    return input_addrs_.contains(addr);
}

void EpochState::store_compute_result_locked(DevAddr addr, niobium::fhetch::Polynomial poly,
                                             uint64_t modulus) noexcept {
    // Pure per-residue storage primitive: this is NOT the op-count choke point.
    // Counting here inflated MRP operations (which fan out to one store per
    // residue) and wrongly charged device-to-device copies (which route through
    // copy_result_locked) as ops. The op counter is now bumped exactly once per
    // high-level operation at each API success point (the SRP/MRP compute
    // templates in compute.hpp and the basis-conversion primitives), so
    // op_count reflects "one per SRP / MRP / basis-convert call, independent of
    // residue fan-out" and copies are accounted only in bytes_d2d (M1).
    poly_map_.insert_or_assign(addr, std::move(poly));
    // This addr now holds a trace-produced value, not a live-in input.
    input_addrs_.erase(addr);
    // A no-modulus (kCopyModulus) result drops any stale entry so a later
    // copy/automorph can't recover a previous occupant's modulus here.
    if (modulus != kCopyModulus)
        addr_modulus_.insert_or_assign(addr, modulus);
    else
        addr_modulus_.erase(addr);
}

uint64_t EpochState::recorded_modulus_locked(DevAddr addr) const noexcept {
    auto it = addr_modulus_.find(addr);
    return it == addr_modulus_.end() ? kCopyModulus : it->second;
}

std::expected<void, HazeInternalError> EpochState::tag_output_locked(DevAddr addr) {
    if (!poly_map_.contains(addr)) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "tag_output_locked: addr not bound in poly_map_");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }
    // An MRP residue tags every residue of its group and promotes the group.
    if (auto rev = addr_to_mrp_groups_.find(addr); rev != addr_to_mrp_groups_.end()) {
        // Registration keeps an addr in at most one group (and erases empty
        // reverse entries), so this set holds exactly one name; the loop is
        // kept as defense in depth should that invariant ever relax.
        assert(rev->second.size() == 1);
        for (const auto &group_name : rev->second) {
            auto g = known_mrp_groups_.find(group_name);
            if (g == known_mrp_groups_.end())
                continue;
            for (DevAddr a : g->second.addrs)
                if (!pending_outputs_.contains(a))
                    pending_outputs_.emplace(a, "haze_out_" + std::to_string(output_counter_++));
            pending_mrp_groups_.insert(group_name);
        }
        return {};
    }
    if (!pending_outputs_.contains(addr))
        pending_outputs_.emplace(addr, "haze_out_" + std::to_string(output_counter_++));
    return {};
}

std::expected<void, HazeInternalError> EpochState::copy_result_locked(DevAddr dst, DevAddr src,
                                                                      uint64_t modulus) noexcept {
    // The op carries the COPY sentinel (the executor lowers ADDI imm=0 at
    // modulus-table index 0 as a register copy); the real modulus rides as
    // metadata. Recover it from the source when the caller passed none.
    auto src_poly = lookup_or_create_locked(src);
    if (!src_poly)
        return std::unexpected(src_poly.error());
    if (modulus == kCopyModulus)
        modulus = recorded_modulus_locked(src);
    auto copy = fhetch::sr_addps(*src_poly, fhetch::Scalar::from_int(0), kCopyModulus);
    if (modulus != kCopyModulus) {
        // Bind the source too (a node only touched by copies would otherwise
        // stay sentinel-bound), and record src's modulus to match the binding
        // so a later copy/automorph of src recovers it.
        fhetch::bind_modulus(*src_poly, modulus);
        fhetch::bind_modulus(copy, modulus);
        addr_modulus_.insert_or_assign(src, modulus);
    }
    store_compute_result_locked(dst, std::move(copy), modulus);
    return {};
}

std::expected<void, HazeInternalError> EpochState::tag_h2d_input_locked(DevAddr addr) noexcept {
    // Both guards below cover invariants that the H2D entry point has
    // already enforced (copy_h2d requires alloc_set_ membership and a
    // configured poly_bytes_, so by the time this runs ring_dim is set
    // and shadow bytes exist). Treat hits as broken-haze internal
    // errors so we don't silently drop the input tag.
    const uint64_t ring_dim = config().ring_dim();
    if (ring_dim == 0) {
        record_internal_error(HazeInternalError::NotConfigured,
                              "tag_h2d_input_locked: ring_dim == 0 after copy_h2d");
        return std::unexpected(HazeInternalError::NotConfigured);
    }
    auto components = allocator().read_polynomial_components(addr, ring_dim);
    if (!components)
        return std::unexpected(components.error());
    fhetch::Polynomial poly =
        fhetch::Polynomial::from_data(std::move(*components), ring_dim, fhetch::Format::Evaluation);
    const std::string name = "haze_in_" + std::to_string(input_counter_++);
    fhetch::tag_input(name, poly);
    // New H2D bytes overwrite the binding, drop any output tag, and reclassify
    // the addr as a live-in input (MRP-group claims stay).
    poly_map_.insert_or_assign(addr, std::move(poly));
    pending_outputs_.erase(addr);
    input_addrs_.insert(addr);
    return {};
}

void EpochState::tag_mrp_input_if_new_locked(const std::string &name, const fhetch::MRP &mrp) {
    // Dedup so each name reaches fhetch exactly once.
    if (auto [it, inserted] = mrp_input_tagged_names_.insert(name); inserted) {
        fhetch::tag_input(*it, mrp);
    }
}

std::expected<void, HazeInternalError>
EpochState::register_mrp_output_group_locked(std::span<const DevAddr> addrs,
                                             std::span<const uint64_t> moduli, std::string &&name) {
    // One device address per modulus; a mismatch is a programming bug in
    // the fan-out helper, surfaced rather than dropped silently.
    if (addrs.size() != moduli.size()) {
        std::ostringstream body;
        body << "register_mrp_output_group_locked('" << name << "'): addrs.size()=" << addrs.size()
             << " != moduli.size()=" << moduli.size();
        record_internal_error(HazeInternalError::MrpGroupAddrModuliMismatch, body.str().c_str());
        return std::unexpected(HazeInternalError::MrpGroupAddrModuliMismatch);
    }
    // Latest-write-wins registration. Identical re-registration (same op
    // re-run, e.g. an in-place accumulation loop) is a cheap no-op; anything
    // else replaces stale group state so a tagged readback assembles the
    // membership of the most recent write, never a stale addr list / moduli.
    auto existing = known_mrp_groups_.find(name);
    if (existing != known_mrp_groups_.end() && existing->second.addrs.size() == addrs.size() &&
        std::equal(addrs.begin(), addrs.end(), existing->second.addrs.begin()) &&
        std::equal(moduli.begin(), moduli.end(), existing->second.moduli.begin())) {
        // Safe to skip the conflict sweep below: any competing registration
        // since this group's last write would have evicted it (existing would
        // be end()), so no other group can claim these addrs right now.
        return {};
    }

    // Any other group claiming one of the new addrs is falsified by this
    // write: its multi-residue claim no longer describes what the addr holds.
    // Evict it wholesale (mirrors invalidate() on free). Members keep their
    // poly_map_ bindings and per-residue tags as standalone SRP values.
    for (DevAddr a : addrs) {
        auto rev = addr_to_mrp_groups_.find(a);
        if (rev == addr_to_mrp_groups_.end())
            continue;
        // Copy: evict_mrp_group_locked edits the reverse map under us.
        std::vector<std::string> conflicting(rev->second.begin(), rev->second.end());
        for (const auto &other : conflicting)
            if (other != name)
                evict_mrp_group_locked(other);
    }

    if (existing != known_mrp_groups_.end()) {
        // Same name, new shape (e.g. an in-place rescale re-using dst[0] with
        // fewer residues): replace membership in place. pending_mrp_groups_
        // stores names, so an already-tagged group exports the replacement.
        for (DevAddr old_addr : existing->second.addrs) {
            auto o = addr_to_mrp_groups_.find(old_addr);
            if (o == addr_to_mrp_groups_.end())
                continue;
            o->second.erase(name);
            if (o->second.empty())
                addr_to_mrp_groups_.erase(o);
        }
        existing->second.addrs.assign(addrs.begin(), addrs.end());
        existing->second.moduli.assign(moduli.begin(), moduli.end());
        for (DevAddr a : addrs)
            addr_to_mrp_groups_[a].insert(name);
        // A tagged group's members each carry a per-residue output tag so
        // flush-time shadow population covers them; extend that to members
        // introduced by the replacement.
        if (pending_mrp_groups_.contains(name)) {
            for (DevAddr a : addrs)
                if (!pending_outputs_.contains(a))
                    pending_outputs_.emplace(a, "haze_out_" + std::to_string(output_counter_++));
        }
        return {};
    }

    auto [it, inserted] = known_mrp_groups_.try_emplace(std::move(name));
    it->second.addrs.assign(addrs.begin(), addrs.end());
    it->second.moduli.assign(moduli.begin(), moduli.end());
    for (DevAddr a : addrs)
        addr_to_mrp_groups_[a].insert(it->first);
    return {};
}

std::expected<void, HazeInternalError> EpochState::tag_pending_outputs_locked() {
    // pending_outputs_ and poly_map_ stay in lockstep via store + invalidate,
    // so a missing binding here is a state-management bug, not recoverable.
    for (auto &[addr, name] : pending_outputs_) {
        auto it = poly_map_.find(addr);
        if (it == poly_map_.end()) {
            std::ostringstream body;
            body << "tag_pending_outputs_locked: pending output '" << name << "' addr 0x"
                 << std::hex << to_uintptr(addr) << std::dec << " missing from poly_map_";
            record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            return std::unexpected(HazeInternalError::MissingPolyMapBinding);
        }
        fhetch::tag_output(name, it->second);
    }

    // Also tag each MRP group as a fhetch MRP output so external callers
    // can pull the multi-residue view via fhetch::result(name, MRP&).
    // pending_mrp_groups_ holds names; resolve through known_mrp_groups_ so
    // the membership exported is the latest registration for that name.
    for (const auto &name : pending_mrp_groups_) {
        auto g_it = known_mrp_groups_.find(name);
        if (g_it == known_mrp_groups_.end()) {
            // pending ⊆ known is maintained by registration/eviction; a miss
            // here is a state-management bug, not recoverable.
            std::ostringstream body;
            body << "tag_pending_outputs_locked: pending MRP group '" << name
                 << "' missing from known_mrp_groups_";
            record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
            return std::unexpected(HazeInternalError::MissingPolyMapBinding);
        }
        const auto &g = g_it->second;
        std::vector<std::pair<fhetch::Polynomial, uint64_t>> pairs;
        pairs.reserve(g.addrs.size());
        for (size_t i = 0; i < g.addrs.size(); ++i) {
            auto it = poly_map_.find(g.addrs[i]);
            if (it == poly_map_.end()) {
                // Group registered but its poly_map_ binding was invalidated before materialize.
                std::ostringstream body;
                body << "tag_pending_outputs_locked: MRP group '" << name << "' addr 0x" << std::hex
                     << to_uintptr(g.addrs[i]) << std::dec << " missing from poly_map_";
                record_internal_error(HazeInternalError::MissingPolyMapBinding, body.str().c_str());
                return std::unexpected(HazeInternalError::MissingPolyMapBinding);
            }
            // Polynomial copy is a shared_ptr refcount bump, not a deep clone.
            pairs.emplace_back(it->second, g.moduli[i]);
        }
        fhetch::tag_output(name, fhetch::MRP::from_pairs(pairs));
    }

    return {};
}

std::expected<void, HazeInternalError> EpochState::finalize_locked(bool run_replay) {
    // G2 state-machine guard: a flush (replay_and_populate) or program write
    // (materialize_only) is illegal while a capture is open — the recorded ops
    // belong to the pending graph, not to an ad-hoc flush. Reject WITHOUT
    // touching state so the in-progress capture survives the rejected call.
    if (capturing_) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "finalize_locked: flush/write attempted during an active capture "
                              "(end the capture first)");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    if (!recording_) {
        return {}; // nothing to finalize
    }

    // No outputs to materialize — recording was opened (e.g., by H2D's
    // eager-tag) but no compute followed. The shadow buffer holds the
    // current bytes; skip the write/replay entirely so the bridge crypto
    // context isn't a prerequisite for compute-free D2H reads.
    if (pending_outputs_.empty() && pending_mrp_groups_.empty()) {
        clear_state_locked();
        return {};
    }

    if (auto tagged = tag_pending_outputs_locked(); !tagged)
        return std::unexpected(tagged.error());

    return do_materialize_locked(run_replay);
}

std::expected<void, HazeInternalError> EpochState::replay_and_populate() noexcept {
    // Open a fresh correlation context for this flush cycle so every diagnostic
    // emitted while draining the epoch -- including those from the replay_bridge
    // on this same thread during the crossing -- shares one id, and bracket the
    // cycle with a trace span. The scope covers the lock, the trace write, the
    // replay dispatch, and shadow population.
    const CorrelationScope cid_scope(next_correlation_id());
    TraceSpan span("epoch.flush");
    HazeLockGuard lock(mutex_);
    auto result = finalize_locked(/*run_replay=*/true);
    if (!result)
        span.mark_error();
    return result;
}

std::expected<void, HazeInternalError> EpochState::materialize_only() noexcept {
    // As replay_and_populate(), but for the record-here / replay-elsewhere path
    // (hazeWriteProgram): correlate and trace the project-dir materialization.
    const CorrelationScope cid_scope(next_correlation_id());
    TraceSpan span("epoch.write_program");
    HazeLockGuard lock(mutex_);
    auto result = finalize_locked(/*run_replay=*/false);
    if (!result)
        span.mark_error();
    return result;
}

bool EpochState::is_recording() noexcept {
    HazeLockGuard lock(mutex_);
    return recording_;
}

std::expected<void, HazeInternalError> EpochState::do_materialize_locked(bool run_replay) {
    if (!recording_) {
        return {};
    }

    // Step 1: write the trace. The replay_bridge post-recording hook
    // runs inside stop_epoch; we drain its failure flag right after.
    const bool stop_ok = CompilerBackend::stop_epoch();
    if (!stop_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "EpochState::do_materialize_locked (stop_epoch)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    if (hazeReplayBridgeTakeHookHadError() != 0) {
        clear_state_locked();
        record_internal_error(
            HazeInternalError::BridgeHookFailed,
            "post_recording_hook reported per-input/output failures (see prior log entries)");
        return std::unexpected(HazeInternalError::BridgeHookFailed);
    }

    // hazeWriteProgram() stops here: step 1 has written the full project dir
    // (.fhetch + inputs + templates + cryptocontext), ready to ship for
    // out-of-process replay (e.g. on the FPGA host). There is no in-process
    // result to read back, so skip replay + shadow population.
    if (!run_replay) {
        clear_state_locked();
        return {};
    }

    // Step 2: dispatch replay. kLocalTarget runs the in-process simulator;
    // other targets spawn nbcc_fhetch_replay over HTTP — both produce
    // serialized_probes/<name>.ct for step 3 to read. Time only this genuine
    // replay dispatch and count the flush unconditionally (a failed replay is
    // still a dispatched flush attempt).
    const auto flush_start = std::chrono::steady_clock::now();
    const bool replay_ok = CompilerBackend::replay();
    const auto flush_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - flush_start)
                              .count();
    metrics().record_flush(static_cast<uint64_t>(flush_ns));
    if (!replay_ok) {
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "EpochState::do_materialize_locked (replay)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Step 3: per-output shadow population. Any failure aborts the
    // epoch so a stale shadow can't surface as a silent wrong-value D2H.
    // fhetch::result() deserializes the replay probe and can throw a cereal
    // exception on a corrupt/truncated file; the try/catch keeps that C++
    // exception from crossing the noexcept C ABI boundary as a raw throw,
    // surfacing a clean HAZE_ERROR_INTERNAL instead.
    for (auto &[addr, name] : pending_outputs_) {
        std::vector<uint64_t> values;
        try {
            fhetch::Polynomial result_poly;
            if (!fhetch::result(name, result_poly)) {
                std::ostringstream body;
                body << "result('" << name << "') unavailable for addr 0x" << std::hex
                     << to_uintptr(addr) << std::dec;
                clear_state_locked();
                record_internal_error(HazeInternalError::BackendOutputMissing, body.str().c_str());
                return std::unexpected(HazeInternalError::BackendOutputMissing);
            }
            if (!extract_polynomial_values(result_poly, name, values)) {
                std::ostringstream body;
                body << "failed to extract values for output '" << name << "' at addr 0x"
                     << std::hex << to_uintptr(addr) << std::dec;
                clear_state_locked();
                record_internal_error(HazeInternalError::BackendOutputDecodeFailed,
                                      body.str().c_str());
                return std::unexpected(HazeInternalError::BackendOutputDecodeFailed);
            }
        } catch (...) {
            clear_state_locked();
            record_internal_error(HazeInternalError::BackendReplayFailed,
                                  "do_materialize_locked: reading replay output threw");
            return std::unexpected(HazeInternalError::BackendReplayFailed);
        }
        if (auto r = allocator().update_shadow(addr, std::move(values)); !r) {
            clear_state_locked();
            return std::unexpected(r.error());
        }
    }

    clear_state_locked();
    return {};
}

void EpochState::clear_state_locked() noexcept {
    poly_map_.clear();
    pending_outputs_.clear();
    addr_modulus_.clear();
    input_addrs_.clear();
    known_mrp_groups_.clear();
    pending_mrp_groups_.clear();
    addr_to_mrp_groups_.clear();
    mrp_input_tagged_names_.clear();
    mrp_in_names_.clear();
    mrp_out_names_.clear();
    recording_ = false;
    input_counter_ = 0;
    output_counter_ = 0;
    mrp_in_name_counter_ = 0;
    mrp_out_name_counter_ = 0;
    // Mirror clears to libnbfhetch so a failed materialise can't leak
    // captures into the next epoch; pairs with EpochSession's setup.
    niobium::compiler().clear_captured();
}

std::expected<void, HazeInternalError> EpochState::begin_capture_locked() noexcept {
    // Nested capture is a caller error; leave the in-progress capture intact.
    if (capturing_) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "begin_capture_locked: capture already active");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // G2 state-machine guard: a capture must start from a clean recording
    // boundary. Uncommitted COMPUTE work (pending SRP outputs or MRP groups
    // recorded but not yet flushed) would otherwise be folded into the captured
    // graph, corrupting its op sequence. A preceding H2D eager-tag (inputs
    // only, no pending outputs) is legitimate and left intact by design.
    if (!pending_outputs_.empty() || !pending_mrp_groups_.empty()) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "begin_capture_locked: uncommitted compute work pending "
                              "(flush before beginning a capture)");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    capturing_ = true;
    // Open a recording for the capture region (no-op if one is already open,
    // e.g. from a preceding H2D eager-tag).
    ensure_recording_locked();
    return {};
}

std::expected<EpochTraceSnapshot, HazeInternalError>
EpochState::end_capture_snapshot_locked() noexcept {
    // No active capture: ending without a matching begin is an illegal-state
    // transition, distinct from ending an empty capture.
    if (!capturing_) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "end_capture_snapshot_locked: no active capture");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Capture opened but nothing was recorded (no compute op emitted an
    // output): mirror the empty-recording contract, leave capture mode
    // cleanly, and report the empty capture instead of failing deeper.
    if (!recording_ || (pending_outputs_.empty() && pending_mrp_groups_.empty())) {
        capturing_ = false;
        clear_state_locked();
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "end_capture_snapshot_locked: nothing recorded during capture");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    // Snapshot the output binding table before any state is cleared; per-residue
    // MRP outputs already carry individual entries in pending_outputs_.
    std::vector<std::pair<DevAddr, std::string>> outputs(pending_outputs_.begin(),
                                                         pending_outputs_.end());

    // G6 ABA defense: capture each output DevAddr's allocation generation NOW,
    // while it is guaranteed live. On every launch, replay_snapshot_locked
    // re-checks the generation before touching the shadow; if the buffer was
    // freed and its DevAddr recycled into a new allocation, the generation will
    // differ and the launch is rejected rather than silently writing a
    // recomputed value into an unrelated buffer. Generation 0 (an address the
    // allocator does not track, e.g. an internal MRP residue that lives for the
    // whole epoch) is stable across launches and compares equal by construction.
    std::vector<uint64_t> output_generations;
    output_generations.reserve(outputs.size());
    for (const auto &[addr, name] : outputs)
        output_generations.push_back(allocator().generation_of(addr));

    // Tag pending SRP + MRP outputs, then write the self-contained project
    // directory (.fhetch + inputs + templates + cryptocontext).
    if (auto tagged = tag_pending_outputs_locked(); !tagged) {
        capturing_ = false;
        clear_state_locked();
        return std::unexpected(tagged.error());
    }
    if (!CompilerBackend::stop_epoch()) {
        capturing_ = false;
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "end_capture_snapshot_locked (stop_epoch)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
    if (hazeReplayBridgeTakeHookHadError() != 0) {
        capturing_ = false;
        clear_state_locked();
        record_internal_error(
            HazeInternalError::BridgeHookFailed,
            "post_recording_hook reported per-input/output failures (see prior log entries)");
        return std::unexpected(HazeInternalError::BridgeHookFailed);
    }

    // G4: securely clone the just-written project into a private, unpredictable,
    // owner-only directory that the graph owns for its lifetime. A later epoch
    // overwriting the default project dir therefore cannot disturb it, and the
    // O_EXCL mkdtemp + 0700 permissions close the predictable-name / world-copy
    // window the previous create_directories+copy path left open. This is
    // capture-only: NO replay is dispatched here (that was the G1 defect). Each
    // launch re-dispatches the frozen project from disk via
    // replay_snapshot_locked, so the graph records the op sequence exactly once
    // and replays it many times.
    const std::filesystem::path src_dir = niobium::compiler().get_program_directory();
    auto cloned = secure_clone_project_dir(src_dir);
    if (!cloned) {
        capturing_ = false;
        clear_state_locked();
        return std::unexpected(cloned.error());
    }
    std::filesystem::path graph_dir = std::move(cloned.value());

    // Best-effort removal of the freshly cloned dir on any subsequent failure so
    // we never leak a half-populated graph directory (G5/G7 partial-state
    // guarantee). Disarmed once the snapshot is successfully constructed.
    auto remove_clone = [&graph_dir]() noexcept {
        std::error_code rm_ec;
        std::filesystem::remove_all(graph_dir, rm_ec);
    };

    // G3 topology fingerprint: capture the recorded instruction sequence so a
    // later hazeGraphExecUpdate can verify the replacement graph is genuinely
    // same-topology (identical op sequence), not merely same output addresses.
    // The trace filename is named by the project index (fhetch_replay.json ->
    // files.instructions). We keep ONLY the non-comment instruction lines: the
    // trace header carries a per-capture timestamp comment, and the FHETCH
    // logical registers (%0, %1, ...) are assigned by operand ORDER rather than
    // by DevAddr, so two captures of the same op sequence with rebound inputs
    // (e.g. dst=a+b vs dst=a+c) yield byte-identical instruction lines while a
    // different opcode (e.g. a multiply) differs — exactly the distinction
    // ExecUpdate must draw. Reading files can throw, so the read is wrapped
    // (noexcept boundary, G5); a missing/empty topology fails the capture rather
    // than producing an un-updatable graph.
    std::string topology;
    try {
        std::ifstream idx_in(graph_dir / "fhetch_replay.json");
        if (idx_in.is_open()) {
            auto j = nlohmann::json::parse(idx_in, nullptr, /*allow_exceptions=*/false);
            if (!j.is_discarded() && j.contains("files")) {
                const std::string trace_file = j["files"].value("instructions", std::string{});
                if (!trace_file.empty()) {
                    std::ifstream tin(graph_dir / trace_file);
                    if (tin.is_open()) {
                        std::ostringstream ss;
                        std::string line;
                        while (std::getline(tin, line)) {
                            // Skip comment lines (they include the capture
                            // timestamp) and blank lines.
                            const auto first = line.find_first_not_of(" \t\r");
                            if (first == std::string::npos || line[first] == '#')
                                continue;
                            ss << line << '\n';
                        }
                        // Canonicalize the FHETCH register tokens (%N) to
                        // first-appearance order. The recorder assigns absolute
                        // register numbers based on the surrounding live set,
                        // which differs between two captures of the SAME op
                        // sequence (e.g. dst=a+b emits `%3,%0,%1` while dst=a+c
                        // emits `%2,%0,%1`). Renumbering by first appearance
                        // collapses that noise while preserving BOTH the opcode
                        // sequence (add vs mul stays distinct) and the data-flow
                        // structure (which operands alias which), so a pure input
                        // rebind compares equal but a different operation does not.
                        const std::string raw = std::move(ss).str();
                        std::unordered_map<std::string, uint64_t> reg_map;
                        std::string canon;
                        canon.reserve(raw.size());
                        for (std::size_t i = 0; i < raw.size();) {
                            if (raw[i] == '%') {
                                std::size_t k = i + 1;
                                while (k < raw.size() && (raw[k] >= '0' && raw[k] <= '9'))
                                    ++k;
                                if (k > i + 1) {
                                    const std::string tok = raw.substr(i, k - i);
                                    const auto ins = reg_map.try_emplace(tok, reg_map.size());
                                    canon += "%r";
                                    canon += std::to_string(ins.first->second);
                                    i = k;
                                    continue;
                                }
                            }
                            canon += raw[i];
                            ++i;
                        }
                        topology = std::move(canon);
                    }
                }
            }
        }
    } catch (...) {
        topology.clear();
    }
    if (topology.empty()) {
        remove_clone();
        capturing_ = false;
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "end_capture_snapshot_locked: project directory copy failed");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Snapshot the input bindings instead of the computed outputs. stop_epoch()
    // above called Compiler::stop(), which ran sync_fhetch_state_to_compiler()
    // to populate the compiler's captured_inputs from this epoch's tag_input
    // records — so for_each_captured_input now enumerates every recorded input
    // (name, kind, per-residue addr_id / modulus / values). We pair each
    // recorded residue with the live HAZE DevAddr that produced it (via the
    // Polynomial's synthetic FHETCH address) so replay can re-read that address's
    // CURRENT shadow on every launch: this is the record-once / replay-many-
    // with-current-inputs contract that a one-time output cache cannot express.
    // Residues with no HAZE shadow behind them (e.g. auto-captured precompute)
    // stay pinned to their recorded values as an auxiliary fallback.
    std::vector<SnapshotInput> inputs;
    try {
        // addr_id -> live-in HAZE DevAddr, over the inputs still bound this
        // epoch. polynomial_address(poly) returns the same synthetic address
        // sync recorded into captured_inputs, so the join is exact.
        std::unordered_map<uint64_t, DevAddr> addr_id_to_dev;
        addr_id_to_dev.reserve(input_addrs_.size());
        for (const DevAddr in_addr : input_addrs_) {
            auto it = poly_map_.find(in_addr);
            if (it == poly_map_.end())
                continue;
            const auto id = static_cast<uint64_t>(niobium::detail::polynomial_address(it->second));
            addr_id_to_dev.emplace(id, in_addr);
        }

        niobium::detail::for_each_captured_input([&](const niobium::CapturedInputRecord &rec) {
            SnapshotInput snap_in;
            snap_in.name = rec.name;
            snap_in.kind = static_cast<uint8_t>(rec.shape.kind);
            // Flatten per_element_moduli back to a per-residue list aligned
            // 1:1 with addr_ids (slice_per_element_moduli concatenates to
            // this order for every kind), and reconstruct the element
            // boundaries store_input_element expects.
            std::vector<uint64_t> flat_moduli;
            for (const auto &element : rec.shape.per_element_moduli)
                flat_moduli.insert(flat_moduli.end(), element.begin(), element.end());
            const bool array_kind = rec.shape.kind == niobium::CapturedKind::SRPArray ||
                                    rec.shape.kind == niobium::CapturedKind::MRPArray;

            snap_in.residues.reserve(rec.addr_ids.size());
            size_t running = 0;
            for (const auto &element : rec.shape.per_element_moduli) {
                for (size_t j = 0; j < element.size(); ++j) {
                    const size_t idx = running + j;
                    if (idx >= rec.addr_ids.size())
                        break;
                    SnapshotInputResidue residue;
                    residue.addr_id = rec.addr_ids[idx];
                    residue.modulus = idx < flat_moduli.size() ? flat_moduli[idx] : 0;
                    residue.starts_new_element = array_kind && j == 0;
                    if (auto dev = addr_id_to_dev.find(residue.addr_id);
                        dev != addr_id_to_dev.end()) {
                        residue.refreshable = true;
                        residue.refresh_addr = dev->second;
                    }
                    // Always retain the recorded values: they seed the first
                    // launch identically to capture and back the fallback for
                    // a residue whose shadow is gone (e.g. a freed input).
                    if (idx < rec.per_residue_values.size())
                        residue.recorded_values = rec.per_residue_values[idx];
                    snap_in.residues.push_back(std::move(residue));
                }
                running += element.size();
            }
            inputs.push_back(std::move(snap_in));
        });
    } catch (...) {
        capturing_ = false;
        clear_state_locked();
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "end_capture_snapshot_locked: input-binding snapshot failed");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Structural signature of the recorded op-sequence, used by
    // graph_exec_update to distinguish a same-topology parameter rebind from a
    // genuine operation-topology change (see trace_signature_for_dir).
    const uint64_t trace_signature = trace_signature_for_dir(graph_dir);

    // Assemble the snapshot. Every member assignment below is a noexcept move
    // (std::filesystem::path, std::string and std::vector all have noexcept
    // move-assignment) and config().target() is itself noexcept, so no
    // exception can escape this final step — the remove_clone guard above has
    // already covered every throwing predecessor (G5).
    EpochTraceSnapshot snapshot;
    snapshot.project_dir = std::move(graph_dir);
    snapshot.outputs = std::move(outputs);
    snapshot.output_generations = std::move(output_generations);
    snapshot.target = config().target();
    snapshot.inputs = std::move(inputs);
    snapshot.trace_signature = trace_signature;

    clear_state_locked();
    capturing_ = false;
    return snapshot;
}

std::expected<void, HazeInternalError>
EpochState::replay_snapshot_locked(const EpochTraceSnapshot &snapshot) noexcept {
    const uint64_t ring_dim = config().ring_dim();
    if (ring_dim == 0) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "replay_snapshot_locked: ring dimension not configured");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    // G2 state-machine guard: a launch is illegal while a capture is open or
    // while any ad-hoc recording (pending SRP/MRP work) is in flight — the
    // launch must not interleave its re-dispatch with a half-recorded epoch.
    // Reject WITHOUT disturbing that state.
    if (capturing_ || !pending_outputs_.empty() || !pending_mrp_groups_.empty()) {
        record_internal_error(HazeInternalError::InvalidArgument,
                              "replay_snapshot_locked: launch attempted during an active "
                              "capture or unflushed recording");
        return std::unexpected(HazeInternalError::InvalidArgument);
    }
    // Index-parallel invariant established by end_capture_snapshot_locked.
    if (snapshot.outputs.size() != snapshot.output_generations.size()) {
        record_internal_error(HazeInternalError::BackendShapeMismatch,
                              "replay_snapshot_locked: outputs / generations size mismatch");
        return std::unexpected(HazeInternalError::BackendShapeMismatch);
    }
    // G6 ABA pre-check (fail fast before the expensive re-dispatch): every output
    // DevAddr must still hold the generation it had at capture. A freed +
    // recycled address now carries a newer generation and is rejected, so a
    // stale graph can never clobber an unrelated live buffer.
    for (size_t i = 0; i < snapshot.outputs.size(); ++i) {
        if (allocator().generation_of(snapshot.outputs[i].first) !=
            snapshot.output_generations[i]) {
            std::ostringstream body;
            body << "replay_snapshot_locked: output addr 0x" << std::hex
                 << to_uintptr(snapshot.outputs[i].first) << std::dec
                 << " was freed/recycled since capture (generation mismatch)";
            record_internal_error(HazeInternalError::SourceUnavailable, body.str().c_str());
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
    }

    // Leave the libnbfhetch compiler's captured_inputs/captured_outputs empty on
    // every exit path. replay() below rehydrates captured_outputs from the
    // snapshot's on-disk <program>.outputs.json (its "cache-hit" path), so a
    // launch would otherwise leave that output binding resident in the shared
    // Compiler singleton. Compiler::start() does NOT clear captured state, so a
    // subsequent hazeStreamBeginCapture would inherit this launch's outputs and
    // write_replay_json() would fold them into the next graph's outputs.json —
    // corrupting that graph's replay (its reconstruct_probes consumes a stale
    // foreign output address and refuses to rewrite the probe). Clearing here
    // mirrors clear_state_locked()'s "don't leak captures into the next epoch"
    // contract for the record-once/replay-many launch path. Guarded because this
    // function is noexcept.
    struct CapturedStateGuard {
        EpochState *self;
        ~CapturedStateGuard() {
            try {
                niobium::fhetch::reset_for_epoch();
                // Drop the EpochState recording caches in lockstep with the
                // fhetch reset above. reset_for_epoch() invalidates
                // libnbfhetch's synthetic-address space, so every cached
                // fhetch::Polynomial still held in poly_map_ (and the
                // input_addrs_ / input-tag-dedup state that mirrors it) is now
                // stale: a later recapture that reused those cached inputs via
                // lookup_or_create_locked's cache-hit path -- without re-tagging
                // them into the freshly reset session -- would collapse distinct
                // operands onto one synthetic address and silently alias inputs.
                // clear_state_locked() drops exactly that state and also calls
                // compiler().clear_captured(), so it subsumes the former
                // stand-alone clear_captured() here and makes the launch mirror
                // the flush path's (do_materialize_locked) "don't leak captures
                // into the next epoch" contract.
                self->clear_state_locked();
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // Best-effort cleanup: the clears manipulate in-memory
                // containers only and are not expected to throw. Swallowing
                // preserves the noexcept boundary; a subsequent launch re-clears
                // at entry regardless.
            }
        }
    } captured_state_guard{this};

    // Re-dispatch the recorded op-sequence against CURRENT inputs. Step 1:
    // start from clean compiler + fhetch capture state so replay()'s internal
    // sync_fhetch_state_to_compiler() is a no-op (the stale tag_input records
    // left over from capture would otherwise double-populate captured_inputs)
    // and re-seed the compiler's captured_inputs ourselves. reset_for_epoch()
    // clears the fhetch input/probe registries; clear_captured() clears the
    // compiler's captured_inputs/outputs.
    niobium::fhetch::reset_for_epoch();
    niobium::compiler().clear_captured();

    // Step 2: repopulate captured_inputs residue-by-residue. Refreshable
    // residues re-read their live device shadow so an operand overwritten at a
    // stable DevAddr between launches takes effect; the read is non-evicting so
    // the input stays reusable for the next launch. A residue whose shadow is
    // gone (freed input) falls back to its recorded values.
    for (const SnapshotInput &input : snapshot.inputs) {
        const auto kind = static_cast<niobium::CapturedKind>(input.kind);
        for (const SnapshotInputResidue &residue : input.residues) {
            std::vector<uint64_t> values;
            bool have_values = false;
            if (residue.refreshable) {
                if (auto shadow =
                        allocator().read_polynomial_components(residue.refresh_addr, ring_dim)) {
                    values = std::move(*shadow);
                    have_values = true;
                }
            }
            if (!have_values)
                values = residue.recorded_values;
            try {
                niobium::compiler().store_input_element(input.name, kind,
                                                        residue.starts_new_element, residue.addr_id,
                                                        residue.modulus, values);
            } catch (...) {
                record_internal_error(HazeInternalError::BackendReplayFailed,
                                      "replay_snapshot_locked: store_input_element threw");
                return std::unexpected(HazeInternalError::BackendReplayFailed);
            }
        }
    }

    // Step 3: point the compiler at this graph's trace and ring dimension, then
    // redirect the sticky last_trace_path by overwriting the active program
    // directory with the snapshot's project (the trace file lands at exactly the
    // path replay() resolves). set_ring_dimension + the copy are wrapped so no
    // filesystem/backend exception escapes this noexcept boundary.
    try {
        niobium::compiler().set_ring_dimension(ring_dim);
        const std::filesystem::path program_dir = niobium::compiler().get_program_directory();
        if (program_dir != snapshot.project_dir) {
            std::filesystem::create_directories(program_dir);
            std::filesystem::copy(snapshot.project_dir, program_dir,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing);
        }
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "replay_snapshot_locked: program directory redirect failed");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Step 4: dispatch replay. CompilerBackend::replay() already contains the C
    // ABI exception guard; a failed replay aborts before any shadow is touched.
    if (!CompilerBackend::replay()) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "replay_snapshot_locked (replay)");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }

    // Step 5: read each freshly-computed output and repopulate its shadow.
    // fhetch::result() deserializes the just-written probe and can throw a
    // cereal exception on a corrupt/truncated probe file; catch it so a clean
    // HAZE_ERROR_INTERNAL crosses the C ABI instead of a C++ exception.
    for (const auto &[addr, name] : snapshot.outputs) {
        std::vector<uint64_t> values;
        try {
            fhetch::Polynomial result_poly;
            if (!fhetch::result(name, result_poly)) {
                std::ostringstream body;
                body << "replay_snapshot_locked: result('" << name << "') unavailable for addr 0x"
                     << std::hex << to_uintptr(addr) << std::dec;
                record_internal_error(HazeInternalError::BackendOutputMissing, body.str().c_str());
                return std::unexpected(HazeInternalError::BackendOutputMissing);
            }
            if (!extract_polynomial_values(result_poly, name, values)) {
                std::ostringstream body;
                body << "replay_snapshot_locked: failed to extract values for '" << name
                     << "' at addr 0x" << std::hex << to_uintptr(addr) << std::dec;
                record_internal_error(HazeInternalError::BackendOutputDecodeFailed,
                                      body.str().c_str());
                return std::unexpected(HazeInternalError::BackendOutputDecodeFailed);
            }
        } catch (...) {
            record_internal_error(HazeInternalError::BackendReplayFailed,
                                  "replay_snapshot_locked: reading replay output threw");
            return std::unexpected(HazeInternalError::BackendReplayFailed);
        }
        // update_shadow runs under the epoch lock, preserving epoch -> allocator
        // lock order.
        if (auto r = allocator().update_shadow(addr, std::move(values)); !r)
            return std::unexpected(r.error());
    }

    // Every output shadow repopulated from the freshly-replayed probes.
    return {};
}

bool EpochState::capturing_locked() const noexcept {
    return capturing_;
}

std::string EpochState::mrp_group_name_locked(bool output, DevAddr leading) {
    auto &names = output ? mrp_out_names_ : mrp_in_names_;
    if (auto it = names.find(leading); it != names.end())
        return it->second;
    auto &counter = output ? mrp_out_name_counter_ : mrp_in_name_counter_;
    std::string name = (output ? "haze_mrp_out_" : "haze_mrp_in_") + std::to_string(counter++);
    names.emplace(leading, name);
    return name;
}

std::expected<void, HazeInternalError> EpochState::tag_output(DevAddr addr) noexcept {
    HazeLockGuard lock(mutex_);
    return tag_output_locked(addr);
}

void EpochState::reset() noexcept {
    HazeLockGuard lock(mutex_);
    clear_state_locked();
    // A device reset abandons any in-progress capture region and zeroes the
    // performance counters.
    capturing_ = false;
    metrics().reset();
}

HazeMutex &EpochSession::init_then_get_mutex() noexcept {
    // Run ensure_initialized() before grabbing the epoch lock so first-call
    // init doesn't serialize; failure surfaces later via is_initialized().
    [[maybe_unused]] const bool _ = backend().ensure_initialized();
    return epoch().mutex_;
}

std::expected<void, HazeInternalError> copy_to_host(void *dst, DevAddr src, size_t count) noexcept {
    return allocator().copy_to_host(dst, src, count);
}

std::expected<void, HazeInternalError> write_program() noexcept {
    return epoch().materialize_only();
}

std::expected<void, HazeInternalError> tag_output(DevAddr addr) noexcept {
    return epoch().tag_output(addr);
}

std::expected<void, HazeInternalError> flush() noexcept {
    // Montgomery / bit-reversed traces can't execute on the in-process
    // simulator. ensure_initialized() already refuses first-time init for this
    // combination (so nothing was recorded); checking again here makes the
    // failure visible at the flush call instead of a silent no-op followed
    // by OutputNotFlushed on the next D2H, and also covers the
    // flags-set-after-init ordering the init-time check can't see.
    if ((config().montgomery() || config().bit_reversal()) && config().target() == kLocalTarget) {
        record_internal_error(HazeInternalError::UnsupportedDataFormat,
                              "haze::flush (montgomery/bit_reversal require a transport "
                              "target such as FUNC_SIM)");
        return std::unexpected(HazeInternalError::UnsupportedDataFormat);
    }
    return epoch().replay_and_populate();
}

RuntimeReadiness runtime_readiness() noexcept {
    // Library-context reinterpretation of a service readiness probe: report
    // whether a crypto context is configured, the compiler backend has
    // initialized, and an epoch is recording. Each field is read under its own
    // subsystem lock (config, then backend's atomic, then epoch), each acquired
    // and released before the next -- no lock is held while another is taken, so
    // this introduces no new lock-ordering edge. The three reads are
    // independent point-in-time observations (readiness is advisory, not a
    // transaction).
    RuntimeReadiness ready{};
    ready.configured = config().configured();
    ready.backend_initialized = backend().is_initialized();
    ready.epoch_active = epoch().is_recording();
    return ready;
}

std::expected<void, HazeInternalError> copy_device_to_device(DevAddr dst, DevAddr src,
                                                             size_t count) noexcept {
    // D2D is a recorded pass-through copy; the source is already tagged (H2D
    // eager-tag or a prior compute), so the copy carries no real modulus.
    //
    // Validate the destination and the byte count BEFORE recording so an
    // invalid copy is never emitted into the trace and never metered (P3). A
    // D2D is a whole-polynomial value copy, so the only supported count is
    // exactly one polynomial; a mismatch is a contract violation. The
    // destination must be a currently-live allocation (generation != 0) — a
    // copy onto a freed or never-allocated address has no owner to bind. The
    // source's liveness and data are validated by copy_result_locked below,
    // which distinguishes an unmapped source (UnknownAddress) from an
    // allocated-but-empty one (SourceUnavailable). These two allocator queries
    // each take (and release) the allocator lock on their own, before the
    // EpochSession acquires the epoch lock, so no lock nesting occurs.
    auto &alloc = allocator();
    const size_t poly_bytes = alloc.polynomial_size();
    if (poly_bytes == 0)
        return std::unexpected(HazeInternalError::NotConfigured);
    if (count != poly_bytes)
        return std::unexpected(HazeInternalError::InvalidArgument);
    if (alloc.generation_of(dst) == 0)
        return std::unexpected(HazeInternalError::UnknownAddress);

    EpochSession session;
    auto result = epoch().copy_result_locked(dst, src);
    // Meter the transfer only after the copy is successfully recorded, and only
    // the exact, validated polynomial byte count (never an attacker-supplied or
    // unvalidated value).
    if (result)
        metrics().add_bytes_d2d(count);
    return result;
}

std::expected<void, HazeInternalError> tag_h2d_input(DevAddr addr) noexcept {
    EpochSession session;
    return epoch().tag_h2d_input_locked(addr);
}

std::expected<std::filesystem::path, HazeInternalError>
secure_clone_project_dir(const std::filesystem::path &src) noexcept {
    // Validate the source is a real directory using the non-throwing overload.
    std::error_code ec;
    if (src.empty() || !std::filesystem::is_directory(src, ec) || ec) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "secure_clone_project_dir: source project directory missing");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    // Create a private, unpredictable destination via mkdtemp: it atomically
    // creates a uniquely-named directory with 0700 permissions (O_EXCL-style,
    // no pre-existing target), closing the predictable-name + world-readable
    // window the old create_directories()+copy(overwrite_existing) path left
    // open (G4). mkdtemp mutates a buffer ending in exactly six 'X'es.
    std::filesystem::path dest;
    try {
        const std::string tmpl =
            (std::filesystem::temp_directory_path() / "haze_graph_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (::mkdtemp(buf.data()) == nullptr) { // NOLINT(misc-include-cleaner)
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "secure_clone_project_dir: mkdtemp failed");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
        dest = std::filesystem::path(buf.data());
    } catch (...) {
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "secure_clone_project_dir: temp path construction failed");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    // Best-effort reclamation of the partially-populated destination on any
    // failure below, so a failed clone never leaks a directory (G5).
    auto cleanup = [&dest]() noexcept {
        std::error_code rm_ec;
        std::filesystem::remove_all(dest, rm_ec);
    };

    // Recursive copy WITHOUT overwrite_existing: the mkdtemp dir is freshly
    // empty, so any name collision would be a genuine error rather than a
    // silent clobber. The error_code overload keeps this non-throwing.
    std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        cleanup();
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "secure_clone_project_dir: recursive copy failed");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    // Re-assert owner-only permissions across the whole cloned tree: copied
    // entries inherit the SOURCE mode bits (potentially group/world readable),
    // so every entry is tightened to owner-only. Directories keep owner_all so
    // they remain traversable for the walk; regular files get owner rw only.
    // The recursive walk can throw, so it is wrapped (noexcept boundary, G5).
    try {
        namespace fs = std::filesystem;
        auto tighten = [](const fs::path &p) noexcept {
            std::error_code pec;
            const bool is_dir = fs::is_directory(p, pec);
            const fs::perms mode =
                is_dir ? fs::perms::owner_all : (fs::perms::owner_read | fs::perms::owner_write);
            fs::permissions(p, mode, fs::perm_options::replace, pec);
        };
        tighten(dest);
        for (fs::recursive_directory_iterator it(dest, ec), end; !ec && it != end;
             it.increment(ec)) {
            tighten(it->path());
        }
        if (ec) {
            cleanup();
            record_internal_error(HazeInternalError::SourceUnavailable,
                                  "secure_clone_project_dir: permission walk failed");
            return std::unexpected(HazeInternalError::SourceUnavailable);
        }
    } catch (...) {
        cleanup();
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "secure_clone_project_dir: exception tightening permissions");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    return dest;
}

} // namespace haze
