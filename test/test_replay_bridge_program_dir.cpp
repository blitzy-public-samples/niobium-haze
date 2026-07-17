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
//
// Regression: the replay bridge must honor a caller-pinned program directory.
//
// hazeReplayBridgeInitCryptoContext() plants the program name "haze" (so the
// project doesn't land under the "niobium_trace" default). set_program_info()
// also resets the project directory to cwd/<name>. A library integrator (e.g.
// FIDESlib's HazeEngine) pins a custom project directory via
// hazeSetProgramDirectory() BEFORE calling the bridge; if the bridge's rename
// resets it, cryptocontext.dat is written under cwd/haze/ while the .fhetch
// trace lands in the pinned dir. nbcc_fhetch_replay --project=<pinned> then
// fails with "Cannot load crypto context — skipping probe serialization" and
// returns no probes, so the transport readback fails.
//
// The existing suites never set a custom program directory (they use the
// cwd/<name> default), so cryptocontext.dat and the trace always coincided and
// the divergence stayed hidden. This test pins a directory outside cwd and
// asserts the bridge preserves it.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <haze/haze.h>          // IWYU pragma: keep
#include <haze/haze_types.h>    // IWYU pragma: keep
#include <haze/replay_bridge.h> // IWYU pragma: keep
#include <niobium/compiler.h>   // IWYU pragma: keep
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr uint64_t kN = 4096;
constexpr uint64_t kQ = 576460752303415297ULL; // standard CKKS test prime
} // namespace

TEST_CASE("replay bridge honors a caller-pinned program directory", "[replay_bridge]") {
    // A project directory that is NOT the cwd/<program_name> default — exactly
    // what a library integrator pins via hazeSetProgramDirectory().
    const fs::path pinned = fs::temp_directory_path() / "haze_replay_bridge_progdir_test";
    std::error_code ec;
    fs::remove_all(pinned, ec);
    REQUIRE(fs::create_directories(pinned, ec));

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);
    // Pin the project directory BEFORE the bridge init, mirroring the HazeEngine
    // bring-up order (hazeSetProgramDirectory precedes the first compute call).
    // This stores the directory in haze::config(); the compiler only learns it at
    // bring-up, so niobium::compiler().get_program_directory() is still the default
    // here — the bridge init is what must propagate it.
    REQUIRE(hazeSetProgramDirectory(pinned.c_str()) == HAZE_SUCCESS);

    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);
    REQUIRE(picked != 0);

    // The bridge's set_program_info("haze") must NOT relocate the project away
    // from the pinned directory. Before the fix this returned cwd/haze, orphaning
    // cryptocontext.dat from the .fhetch trace.
    CHECK(niobium::compiler().get_program_directory() == pinned);

    // And cryptocontext.dat must land in the pinned directory (where the trace
    // and templates are written), so nbcc_fhetch_replay --project=<pinned> finds it.
    CHECK(fs::exists(pinned / "cryptocontext.dat"));

    hazeReplayBridgeReset();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(pinned, ec);
}

// P7-FS-SEC-02: the simulator writes ciphertext/template/context scratch into
// the program directory. Those artifacts must be owner-only (files 0600,
// directories 0700) so they are never group/world readable, even under a
// permissive umask. Restores the ambient umask on scope exit (RAII) so a
// failed assertion cannot leak the relaxed mask into later cases.
namespace {
struct UmaskGuard {
    mode_t old_mask;
    explicit UmaskGuard(mode_t mask) noexcept : old_mask(umask(mask)) {}
    ~UmaskGuard() { umask(old_mask); }
    UmaskGuard(const UmaskGuard &) = delete;
    UmaskGuard &operator=(const UmaskGuard &) = delete;
};
} // namespace

TEST_CASE("materialized program-directory artifacts are owner-only (0600/0700)", "[integration]") {
    const fs::path pinned = fs::temp_directory_path() / "haze_fs_sec02_perms_test";
    std::error_code ec;
    fs::remove_all(pinned, ec);
    REQUIRE(fs::create_directories(pinned, ec));

    // Most permissive umask: prove the runtime tightens artifacts itself rather
    // than inheriting a restrictive process mask.
    const UmaskGuard umask_guard(0);

    constexpr size_t kBytes = static_cast<size_t>(kN) * sizeof(uint64_t);
    std::vector<uint64_t> ha(kN);
    std::vector<uint64_t> hb(kN);
    for (uint64_t i = 0; i < kN; ++i) {
        ha[i] = (i % 101) + 1;
        hb[i] = (i % 97) + 2;
    }

    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    REQUIRE(hazeSetProgramDirectory(pinned.c_str()) == HAZE_SUCCESS);
    REQUIRE(hazeSetReducedNoise(1) == HAZE_SUCCESS);
    REQUIRE(hazeSetRingDimension(kN) == HAZE_SUCCESS);
    uint64_t picked = 0;
    REQUIRE(hazeReplayBridgeInitCryptoContext(kN, kQ, &picked) == HAZE_SUCCESS);
    REQUIRE(hazeSetCiphertextModulus(0, kQ) == HAZE_SUCCESS);
    REQUIRE(hazeConfigureDevice() == HAZE_SUCCESS);

    void *a = nullptr;
    void *b = nullptr;
    void *d = nullptr;
    REQUIRE(hazeMalloc(&a, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&b, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMalloc(&d, kBytes) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(a, ha.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeMemcpy(b, hb.data(), kBytes, HAZE_MEMCPY_HOST_TO_DEVICE) == HAZE_SUCCESS);
    REQUIRE(hazeAdd(d, a, b, 0, nullptr) == HAZE_SUCCESS);
    REQUIRE(hazeTagOutput(d) == HAZE_SUCCESS);
    REQUIRE(hazeFlush() == HAZE_SUCCESS);

    // Walk the whole materialized tree: every directory 0700, every regular
    // file 0600 — no group or world bits anywhere.
    const fs::path progdir = niobium::compiler().get_program_directory();
    const fs::perms kFileMode = fs::perms::owner_read | fs::perms::owner_write;
    const fs::perms kDirMode = fs::perms::owner_all;
    size_t files = 0;
    size_t dirs = 0;
    for (fs::recursive_directory_iterator it(progdir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::file_status st = fs::symlink_status(it->path(), ec);
        REQUIRE_FALSE(ec);
        if (fs::is_directory(st)) {
            ++dirs;
            CHECK((st.permissions() & fs::perms::all) == kDirMode);
        } else if (fs::is_regular_file(st)) {
            ++files;
            CHECK((st.permissions() & fs::perms::all) == kFileMode);
        }
    }
    // The program directory root itself must also be tightened to 0700.
    CHECK((fs::status(progdir, ec).permissions() & fs::perms::all) == kDirMode);
    // A real flush must have written artifacts and descended into at least one
    // subdirectory (ciphertext_templates/serialized_probes), otherwise the
    // file/dir mode checks above would be vacuous.
    CHECK(files > 0);
    CHECK(dirs > 0);

    hazeReplayBridgeReset();
    REQUIRE(hazeDeviceReset() == HAZE_SUCCESS);
    fs::remove_all(pinned, ec);
}
