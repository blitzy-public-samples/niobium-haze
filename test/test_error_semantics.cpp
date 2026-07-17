// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
#include "common/errors.hpp"
#include "integration_helpers.hpp"

#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <haze/haze.h>       // IWYU pragma: keep
#include <haze/haze_types.h> // IWYU pragma: keep
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

TEST_CASE("error semantics: the last error clears after being read", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(nullptr, 32768) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeGetLastError() == HAZE_SUCCESS);
}

TEST_CASE("error semantics: the last error register is thread-local", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeGetDeviceProperties(nullptr, 0) == HAZE_ERROR_INVALID_VALUE);

    bool other_thread_saw_clean = false;
    std::thread t([&other_thread_saw_clean] {
        other_thread_saw_clean = (hazeGetLastError() == HAZE_SUCCESS);
    });
    t.join();

    REQUIRE(other_thread_saw_clean);
    REQUIRE(hazeGetLastError() == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();
}

TEST_CASE("error semantics: every public error code maps to a non-null string", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    const hazeError_t codes[] = {
        HAZE_SUCCESS,
        HAZE_ERROR_INVALID_VALUE,
        HAZE_ERROR_OUT_OF_MEMORY,
        HAZE_ERROR_NOT_SUPPORTED,
        HAZE_ERROR_CONFIGERR,
        HAZE_ERROR_UNKNOWN_ADDRESS,
        HAZE_ERROR_NO_DATA,
        HAZE_ERROR_ALLOC_TOO_SMALL,
        HAZE_ERROR_SOURCE_UNAVAILABLE,
        HAZE_ERROR_NOT_FLUSHED,
        HAZE_ERROR_INTERNAL,
    };
    for (const hazeError_t code : codes) {
        const char *message = hazeGetErrorString(code);
        REQUIRE(message != nullptr);
        REQUIRE(!std::string_view(message).empty());
    }
}

TEST_CASE("error semantics: an unknown error code yields a stable non-null fallback", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    int raw = 999;
    hazeError_t bogus{};
    static_assert(sizeof(hazeError_t) == sizeof(raw), "enum size mismatch");
    std::memcpy(&bogus, &raw, sizeof(bogus));
    const char *message = hazeGetErrorString(bogus);
    REQUIRE(message != nullptr);
    REQUIRE(std::string_view(message) == "unknown error");
}

TEST_CASE("error semantics: each internal error variant maps to its exact public code", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    using haze::HazeInternalError;

    // The expected column mirrors the authoritative switch in
    // src/common/errors.cpp to_public_error(); an incorrect mapping now fails
    // the exact-equality REQUIRE instead of being accepted as "some defined
    // code". All eighteen variants are listed so a new variant that is added
    // without a mapping will not compile past the static_assert below.
    struct Mapping {
        HazeInternalError internal;
        hazeError_t expected_public;
    };
    const Mapping table[] = {
        {.internal = HazeInternalError::InvalidArgument,
         .expected_public = HAZE_ERROR_INVALID_VALUE},
        {.internal = HazeInternalError::NotConfigured, .expected_public = HAZE_ERROR_CONFIGERR},
        {.internal = HazeInternalError::UnknownAddress,
         .expected_public = HAZE_ERROR_UNKNOWN_ADDRESS},
        {.internal = HazeInternalError::NoData, .expected_public = HAZE_ERROR_NO_DATA},
        {.internal = HazeInternalError::AllocTooSmall,
         .expected_public = HAZE_ERROR_ALLOC_TOO_SMALL},
        {.internal = HazeInternalError::BackendInitFailed, .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::BackendReplayFailed,
         .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::BackendShapeMismatch,
         .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::MrpGroupAddrModuliMismatch,
         .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::MissingPolyMapBinding,
         .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::ShadowSizeMismatch, .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::BackendOutputMissing,
         .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::BackendOutputDecodeFailed,
         .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::BridgeHookFailed, .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::PoolMapDesync, .expected_public = HAZE_ERROR_INTERNAL},
        {.internal = HazeInternalError::SourceUnavailable,
         .expected_public = HAZE_ERROR_SOURCE_UNAVAILABLE},
        {.internal = HazeInternalError::OutputNotFlushed,
         .expected_public = HAZE_ERROR_NOT_FLUSHED},
        {.internal = HazeInternalError::UnsupportedDataFormat,
         .expected_public = HAZE_ERROR_NOT_SUPPORTED},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == 18U,
                  "all 18 HazeInternalError variants must be enumerated");
    for (const Mapping &entry : table) {
        const hazeError_t pub = haze::to_public_error(entry.internal);
        REQUIRE(pub == entry.expected_public);
        REQUIRE(hazeGetErrorString(pub) != nullptr);
    }
}

TEST_CASE("error semantics: invalid arguments never throw across the C ABI", "[unit]") {
    // REQUIRE_NOTHROW cannot observe an exception crossing a noexcept C ABI
    // function: the runtime calls std::terminate before any handler runs, so a
    // same-process check would abort the whole test binary instead of failing
    // one assertion. Fork instead, drive a battery of pathological inputs
    // through the noexcept ABI in the child, and require the child to exit
    // normally with status 0. A non-zero status means a translated error code
    // was wrong; abnormal termination (WIFEXITED false) means an exception
    // escaped and std::terminate fired.
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        int rc = 0;
        if (hazeDeviceReset() != HAZE_SUCCESS) {
            rc = 2;
        } else if (hazeMalloc(nullptr, 0) != HAZE_ERROR_INVALID_VALUE) {
            rc = 3;
        } else if (hazeMalloc(nullptr, 32768) != HAZE_ERROR_INVALID_VALUE) {
            rc = 4;
        } else if (hazeFree(nullptr) != HAZE_SUCCESS) {
            rc = 5;
        } else if (hazeMemcpy(nullptr, nullptr, 0, HAZE_MEMCPY_HOST_TO_DEVICE) !=
                   HAZE_ERROR_INVALID_VALUE) {
            rc = 6;
        } else if (hazeGetDeviceProperties(nullptr, 99) != HAZE_ERROR_INVALID_VALUE) {
            rc = 7;
        } else if (hazeTagOutput(nullptr) != HAZE_ERROR_INVALID_VALUE) {
            rc = 8;
        } else if (hazeHostAlloc(nullptr, 4096, 0) != HAZE_ERROR_INVALID_VALUE) {
            rc = 9;
        }
        _exit(rc);
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    // WIFEXITED / WEXITSTATUS are provided by <sys/wait.h>; include-cleaner
    // attributes the glibc macros to <bits/waitstatus.h>, so its misattribution
    // is suppressed here in the same way the rest of the tree does for POSIX.
    // NOLINTNEXTLINE(misc-include-cleaner)
    const bool exited_normally = WIFEXITED(status);
    // NOLINTNEXTLINE(misc-include-cleaner)
    const int exit_code = exited_normally ? WEXITSTATUS(status) : -1;
    REQUIRE(exited_normally);
    REQUIRE(exit_code == 0);
}

TEST_CASE("error semantics: graph capture survives a filesystem failure without throwing across "
          "the C ABI",
          "[integration]") {
    // Reaching the graph's secure clone requires a real recorded op, so the
    // backend is configured and the capture opened HERE in the parent (context
    // build may run a thread pool). The child is forked with the capture
    // already pending and does nothing but the faulting end-capture -- which is
    // capture-only (trace serialization + filesystem clone, no replay/compute),
    // so no thread-pool work runs after fork. The fault points the temp-dir
    // resolver at "/dev/null" (never a directory), so the clone (mkdtemp under
    // temp_directory_path()) fails. A correct noexcept boundary translates that
    // to an error code; an exception escaping it would call std::terminate, so
    // the child would not exit normally with status 0.
    (void)haze::test::setup_integration_compute_config(4096);
    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> host(4096, 1ULL);
    REQUIRE(hazeMemcpy(a, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        setenv("TMPDIR", "/dev/null", 1); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
        hazeGraph_t graph = nullptr;
        const hazeError_t ec = hazeStreamEndCapture(nullptr, &graph);
        if (ec == HAZE_SUCCESS) {
            (void)hazeGraphDestroy(graph); // fault did not trigger; stay deterministic
            _exit(20);
        }
        _exit(0); // clone failed cleanly; no exception crossed the noexcept ABI
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    // NOLINTNEXTLINE(misc-include-cleaner)
    const bool exited_normally = WIFEXITED(status);
    // NOLINTNEXTLINE(misc-include-cleaner)
    const int exit_code = exited_normally ? WEXITSTATUS(status) : -1;
    REQUIRE(exited_normally);
    REQUIRE(exit_code == 0);

    // The parent still holds the pending capture (fork copied it); close and
    // discard it with a working temp dir, then reset for the next case.
    hazeGraph_t parent_graph = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &parent_graph) == HAZE_SUCCESS);
    REQUIRE(parent_graph != nullptr);
    REQUIRE(hazeGraphDestroy(parent_graph) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

TEST_CASE("error semantics: graph exec update survives a filesystem failure without throwing "
          "across the C ABI",
          "[integration]") {
    // As above, but the fault is injected at hazeGraphExecUpdate time: a fully
    // valid graph and exec are built in the parent, then the child forks and
    // runs only the update under a broken temp dir, so the update's secure
    // clone fails. G7 guarantees the exec is left valid (the old on-disk copy
    // is never removed before the replacement is built) and G5 that no
    // exception escapes, so the child must still exit normally with status 0.
    (void)haze::test::setup_integration_compute_config(4096);
    constexpr std::size_t kBytes = 4096 * sizeof(uint64_t);
    void *a = nullptr;
    void *b = nullptr;
    void *dst = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&dst, kBytes) == HAZE_SUCCESS);
    const std::vector<uint64_t> host(4096, 1ULL);
    REQUIRE(hazeMemcpy(a, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, host.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeStreamBeginCapture(nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(dst, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(dst) == HAZE_SUCCESS);
    hazeGraph_t graph = nullptr;
    REQUIRE(hazeStreamEndCapture(nullptr, &graph) == HAZE_SUCCESS);
    REQUIRE(graph != nullptr);
    hazeGraphExec_t exec = nullptr;
    REQUIRE(hazeGraphInstantiate(&exec, graph) == HAZE_SUCCESS);
    REQUIRE(exec != nullptr);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        setenv("TMPDIR", "/dev/null", 1); // NOLINT(misc-include-cleaner) POSIX, via <cstdlib>
        const hazeError_t ec = hazeGraphExecUpdate(exec, graph);
        _exit(ec == HAZE_SUCCESS ? 20 : 0);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    // NOLINTNEXTLINE(misc-include-cleaner)
    const bool exited_normally = WIFEXITED(status);
    // NOLINTNEXTLINE(misc-include-cleaner)
    const int exit_code = exited_normally ? WEXITSTATUS(status) : -1;
    REQUIRE(exited_normally);
    REQUIRE(exit_code == 0);

    // The child's failed update never touched the parent's on-disk exec (G7),
    // so with a working temp dir the same update succeeds and leaves a
    // launchable exec -- proving the failure path was side-effect free.
    REQUIRE(hazeGraphExecUpdate(exec, graph) == HAZE_SUCCESS);
    REQUIRE(hazeGraphLaunch(exec, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeGraphExecDestroy(exec) == HAZE_SUCCESS);
    REQUIRE(hazeGraphDestroy(graph) == HAZE_SUCCESS);
    REQUIRE(hazeFree(a) == HAZE_SUCCESS);
    REQUIRE(hazeFree(b) == HAZE_SUCCESS);
    REQUIRE(hazeFree(dst) == HAZE_SUCCESS);
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// P7-ABI-01 regression: hostile / overflowing modulus counts.
//
// Every MRP and basis-conversion entry point takes a caller-supplied residue
// count (base_len / src_base_len / digit_count / ...). Before the fix an
// absurd value (e.g. SIZE_MAX) walked the shim past its short pointer/base
// arrays and drove a std::vector::reserve(count) to throw std::length_error,
// which escaped these HAZE_NOEXCEPT C-ABI functions and aborted the process
// (and tripped ASan/UBSan on the intermediate out-of-bounds reads). The guards
// now cap every such count at haze::kMaxCiphertextModuli (64 -- the device
// modulus envelope) and return HAZE_ERROR_INVALID_VALUE at the boundary.
//
// These cases run under ASan/UBSan (build-asan / the sanitizers workflow): the
// base/scalar/pointer arrays below are deliberately only three elements long,
// so if any guard ever regressed to iterate the hostile length the sanitizers
// would fault on the over-read before the assertion could even be reached.
// ---------------------------------------------------------------------------
TEST_CASE("P7-ABI-01: MRP and basis-conversion ops reject a hostile modulus "
          "count at the ABI boundary",
          "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);

    // Short, real backing storage. The guards fire before any of these are
    // dereferenced, so no device configuration is required to reach them.
    uint64_t base3[3] = {0x1ULL, 0x3ULL, 0x5ULL};
    uint64_t scalars3[3] = {0x2ULL, 0x4ULL, 0x6ULL};
    std::size_t digit_lens3[3] = {1, 1, 1};
    // Distinct 3-element backing arrays for the destination and the two
    // sources. The guards fire before any element is dereferenced, but keeping
    // real short storage means a regressed guard that iterated the hostile
    // length would over-read these arrays and fault under ASan.
    uint64_t dst_backing[3] = {0, 0, 0};
    uint64_t src1_backing[3] = {0, 0, 0};
    uint64_t src2_backing[3] = {0, 0, 0};
    void *dst[3] = {&dst_backing[0], &dst_backing[1], &dst_backing[2]};
    const void *src1[3] = {&src1_backing[0], &src1_backing[1], &src1_backing[2]};
    const void *src2[3] = {&src2_backing[0], &src2_backing[1], &src2_backing[2]};

    // Just over the envelope (65) and the pathological extreme (SIZE_MAX). Both
    // must be rejected identically -- the bound is an inclusive '<= 64'.
    for (const std::size_t bad : {static_cast<std::size_t>(65), SIZE_MAX}) {
        INFO("hostile modulus count = " << bad);

        // Compute MRP: pairwise ops (base_len is the last count argument).
        REQUIRE(hazeAddMrp(dst, src1, src2, base3, bad, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeSubMrp(dst, src1, src2, base3, bad, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeMulMrp(dst, src1, src2, base3, bad, nullptr) == HAZE_ERROR_INVALID_VALUE);

        // Compute MRP: scalar ops.
        REQUIRE(hazeAddScalarMrp(dst, src1, scalars3, base3, bad, nullptr) ==
                HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeSubScalarMrp(dst, src1, scalars3, base3, bad, nullptr) ==
                HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeMulScalarMrp(dst, src1, scalars3, base3, bad, nullptr) ==
                HAZE_ERROR_INVALID_VALUE);

        // Compute MRP: transforms and automorphisms.
        REQUIRE(hazeNTTMrp(dst, src1, base3, bad, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeINTTMrp(dst, src1, base3, bad, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeAutomorphMrp(dst, src1, 5, base3, bad, nullptr) == HAZE_ERROR_INVALID_VALUE);
        REQUIRE(hazeRotAutomorphCoeffMrp(dst, src1, 1, base3, bad, nullptr) ==
                HAZE_ERROR_INVALID_VALUE);

        // Memory MRP: base_len is the residue count (hostile); `count` here is
        // the legitimate per-residue byte size and stays valid.
        REQUIRE(hazeMemcpyMrp(dst, src1, 32768, HAZE_MEMCPY_DEVICE_TO_DEVICE, base3, bad) ==
                HAZE_ERROR_INVALID_VALUE);

        // Basis conversion: a hostile length in either the source or the
        // destination base must be rejected.
        const hazeBasisConvertParams bc_src{base3, bad, base3, 3};
        REQUIRE(hazeBasisConvert(dst, src1, &bc_src, nullptr) == HAZE_ERROR_INVALID_VALUE);
        const hazeBasisConvertParams bc_dst{base3, 3, base3, bad};
        REQUIRE(hazeBasisConvert(dst, src1, &bc_dst, nullptr) == HAZE_ERROR_INVALID_VALUE);

        // Mod-down: hostile source or rescale base length.
        const hazeModDownParams md_src{base3, bad, base3, 1};
        REQUIRE(hazeModDown(dst, src1, &md_src, nullptr) == HAZE_ERROR_INVALID_VALUE);
        const hazeModDownParams md_rescale{base3, 3, base3, bad};
        REQUIRE(hazeModDown(dst, src1, &md_rescale, nullptr) == HAZE_ERROR_INVALID_VALUE);

        // Mod-up: hostile digit_count (the pre-fix crash walked digit_base_lens)
        // and hostile src_base_len / p_base_len all rejected before the loop.
        const hazeModUpParams mu_digits{base3, 3, base3, 3, digit_lens3, bad, base3, 3};
        REQUIRE(hazeModUp(dst, src1, &mu_digits, nullptr) == HAZE_ERROR_INVALID_VALUE);
        const hazeModUpParams mu_src{base3, bad, base3, 3, digit_lens3, 3, base3, 3};
        REQUIRE(hazeModUp(dst, src1, &mu_src, nullptr) == HAZE_ERROR_INVALID_VALUE);

        (void)hazeGetLastError();
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}

// ---------------------------------------------------------------------------
// P7-ABI-01 boundary regression: prove the cap is inclusive at 64 and rejects
// at 65 / SIZE_MAX, so the fix hardens the overflow path without shrinking the
// legitimate device modulus envelope. The MRP allocator path is self-contained
// (a configured ring dimension is its only prerequisite), giving an
// unambiguous accept/reject signal driven solely by the count bound.
// ---------------------------------------------------------------------------
TEST_CASE("P7-ABI-01: the modulus-count bound accepts 64 and rejects 65 and SIZE_MAX", "[unit]") {
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(4096) == HAZE_SUCCESS);
    constexpr std::size_t kPolyBytes = static_cast<std::size_t>(4096) * sizeof(uint64_t);

    // 64 == kMaxCiphertextModuli: a legal MRP group size. The cap is strictly
    // '>', so the boundary allocation succeeds and yields 64 live addresses.
    std::array<void *, 64> ptrs64{};
    REQUIRE(hazeMallocMrp(ptrs64.data(), ptrs64.size(), kPolyBytes) == HAZE_SUCCESS);
    for (void *p : ptrs64)
        REQUIRE(p != nullptr);
    REQUIRE(hazeFreeMrp(ptrs64.data(), ptrs64.size()) == HAZE_SUCCESS);

    // 65 and SIZE_MAX are over the envelope: rejected before any reservation,
    // and the output array is left untouched (no partial writes on rejection).
    void *out = nullptr;
    REQUIRE(hazeMallocMrp(&out, 65, kPolyBytes) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(out == nullptr);
    REQUIRE(hazeMallocMrp(&out, SIZE_MAX, kPolyBytes) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(out == nullptr);
    (void)hazeGetLastError();

    // The free path enforces the same bound.
    REQUIRE(hazeFreeMrp(&out, 65) == HAZE_ERROR_INVALID_VALUE);
    REQUIRE(hazeFreeMrp(&out, SIZE_MAX) == HAZE_ERROR_INVALID_VALUE);
    (void)hazeGetLastError();

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
}
