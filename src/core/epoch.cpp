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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <haze/replay_bridge.h>
#include <ios>
#include <niobium/compiler.h>
#include <niobium/fhetch_api.h>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

// Disk-driven local FHETCH replay entry point. Defined in libnbfhetch
// (src/local_replay.cpp) and absorbed into libhaze; it drives the simulator
// purely from an on-disk project directory with no Compiler-singleton state,
// which is exactly what graph replay-many requires: each launch re-dispatches
// the frozen captured project deterministically. Self-declared here (rather
// than pulling a private libnbfhetch header) to keep the include surface
// minimal; the mangled niobium:: symbol stays internal to libhaze under the
// version script, so the symbol-leak audit is unaffected.
namespace niobium {
bool run_local_replay_from_project(const std::filesystem::path &dir);
} // namespace niobium

namespace haze {

namespace fhetch = niobium::fhetch;

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
    for (auto &[addr, name] : pending_outputs_) {
        fhetch::Polynomial result_poly;
        if (!fhetch::result(name, result_poly)) {
            std::ostringstream body;
            body << "result('" << name << "') unavailable for addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            clear_state_locked();
            record_internal_error(HazeInternalError::BackendOutputMissing, body.str().c_str());
            return std::unexpected(HazeInternalError::BackendOutputMissing);
        }
        std::vector<uint64_t> values;
        if (!extract_polynomial_values(result_poly, name, values)) {
            std::ostringstream body;
            body << "failed to extract values for output '" << name << "' at addr 0x" << std::hex
                 << to_uintptr(addr) << std::dec;
            clear_state_locked();
            record_internal_error(HazeInternalError::BackendOutputDecodeFailed, body.str().c_str());
            return std::unexpected(HazeInternalError::BackendOutputDecodeFailed);
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
        record_internal_error(HazeInternalError::SourceUnavailable,
                              "end_capture_snapshot_locked: could not read instruction trace "
                              "for topology fingerprint");
        return std::unexpected(HazeInternalError::SourceUnavailable);
    }

    // Assemble the snapshot. Every member assignment below is a noexcept move
    // (std::filesystem::path, std::string and std::vector all have noexcept
    // move-assignment) and config().target() is itself noexcept, so no
    // exception can escape this final step — the remove_clone guard above has
    // already covered every throwing predecessor (G5).
    EpochTraceSnapshot snapshot;
    snapshot.project_dir = std::move(graph_dir);
    snapshot.outputs = std::move(outputs);
    snapshot.target = config().target();
    snapshot.topology = std::move(topology);
    snapshot.output_generations = std::move(output_generations);

    clear_state_locked();
    capturing_ = false;
    return snapshot;
}

std::expected<void, HazeInternalError>
EpochState::replay_snapshot_locked(const EpochTraceSnapshot &snapshot) noexcept {
    // G2 state-machine guard: a launch is illegal while a capture is open or
    // while any ad-hoc recording (pending SRP/MRP work) is in flight — the
    // launch must not interleave its disk-driven replay with a half-recorded
    // epoch. Reject WITHOUT disturbing that state.
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

    // G6 ABA pre-check (fail fast before the expensive replay): every output
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

    // G1 real replay-many: re-dispatch the frozen captured project from disk.
    // run_local_replay_from_project drives the simulator purely from the
    // on-disk directory with no Compiler-singleton state, so it is repeatable
    // and independent of whatever epoch ran last. G5: it (and the JSON readback
    // below) can throw through OpenFHE / nlohmann / filesystem, so the whole
    // dispatch+readback is wrapped; nothing escapes this noexcept boundary.
    try {
        // A launch materializes real values, exactly like a flush; time it and
        // count it so the performance counters see graph launches as the
        // genuine dispatches they are (mirrors do_materialize_locked).
        const auto flush_start = std::chrono::steady_clock::now();
        const bool replay_ok = niobium::run_local_replay_from_project(snapshot.project_dir);
        const auto flush_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - flush_start)
                                  .count();
        metrics().record_flush(static_cast<uint64_t>(flush_ns));
        if (!replay_ok) {
            record_internal_error(HazeInternalError::BackendReplayFailed,
                                  "replay_snapshot_locked: run_local_replay_from_project failed");
            return std::unexpected(HazeInternalError::BackendReplayFailed);
        }

        // Read the freshly computed outputs the replay wrote to disk. Schema:
        // { "outputs": [ { "name":..., "elements":[ { "addr_id":u64,
        //   "modulus":u64, "status":"computed"|"missing", "values":[u64...] } ] } ] }
        //
        // Match by output NAME, not by addr_id: addr_id here is a FHETCH
        // logical index assigned by the simulator (e.g. 2), NOT the HAZE
        // DevAddr, so the two address spaces do not coincide. This mirrors the
        // in-process read-back path (do_materialize_locked keys results by
        // name via fhetch::result); we take the first computed element of each
        // named output, matching extract_polynomial_values' first-tower
        // semantics, and bind it to the DevAddr the snapshot recorded.
        std::ifstream in(snapshot.project_dir / "fhetch_replay_outputs.json");
        if (!in.is_open()) {
            record_internal_error(HazeInternalError::BackendOutputMissing,
                                  "replay_snapshot_locked: fhetch_replay_outputs.json missing");
            return std::unexpected(HazeInternalError::BackendOutputMissing);
        }
        auto root = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (root.is_discarded() || !root.contains("outputs")) {
            record_internal_error(HazeInternalError::BackendOutputDecodeFailed,
                                  "replay_snapshot_locked: malformed fhetch_replay_outputs.json");
            return std::unexpected(HazeInternalError::BackendOutputDecodeFailed);
        }
        std::unordered_map<std::string, std::vector<uint64_t>> by_name;
        for (const auto &out_entry : root["outputs"]) {
            const auto name = out_entry.value("name", std::string{});
            if (name.empty() || by_name.contains(name))
                continue;
            for (const auto &elem : out_entry.value("elements", nlohmann::json::array())) {
                if (elem.value("status", std::string{}) != "computed")
                    continue;
                by_name.emplace(name, elem.value("values", std::vector<uint64_t>{}));
                break; // first computed element (first tower), matching in-process readback
            }
        }

        // Repopulate each output shadow from the recomputed values. update_shadow
        // runs under the epoch lock, preserving the epoch -> allocator order.
        for (const auto &[addr, name] : snapshot.outputs) {
            auto it = by_name.find(name);
            if (it == by_name.end()) {
                std::ostringstream body;
                body << "replay_snapshot_locked: no computed value for output '" << name
                     << "' at addr 0x" << std::hex << to_uintptr(addr) << std::dec;
                record_internal_error(HazeInternalError::BackendOutputMissing, body.str().c_str());
                return std::unexpected(HazeInternalError::BackendOutputMissing);
            }
            // Copy (not move): several snapshot outputs may legitimately share a
            // name (e.g. MRP residues), so each by_name entry can be consumed
            // more than once.
            std::vector<uint64_t> values = it->second;
            if (auto r = allocator().update_shadow(addr, std::move(values)); !r)
                return std::unexpected(r.error());
        }
        return {};
    } catch (...) {
        record_internal_error(HazeInternalError::BackendReplayFailed,
                              "replay_snapshot_locked: exception during replay/readback");
        return std::unexpected(HazeInternalError::BackendReplayFailed);
    }
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
