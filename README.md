# Haze — CUDA-like API for Niobium FHE

A C library that exposes a **CUDA-like** API for the **Niobium Mistic** FHE
accelerator. Library authors write `hazeMalloc` / `hazeMemcpy` / `hazeAdd` /
`hazeNTT` calls the same way they would write `cudaMalloc` / `cudaMemcpy` /
custom CUDA kernels, and haze records every operation as an unoptimized FHETCH
Polynomial IR trace that is then executed locally (for validation) or shipped to
the Niobium compilation service for optimization and deployment to hardware.

Where [`niobium-client`](https://github.com/NiobiumInc/niobium-client) integrates
at the OpenFHE level (probes intercept `EvalMult`, `EvalAdd`, ...) haze
integrates one layer below: each public entry point is a single polynomial-level
op (`NTT`, `MUL`, `ADDP`, basis convert, ...). The shape is deliberately CUDA's
so that GPU FHE libraries written against CUDA — for example
[FIDESlib](https://github.com/CAPS-UMU/FIDESlib) — can be retargeted to
Niobium hardware with minimal porting effort.

For the FHETCH Polynomial IR instruction set, session API, trace format, and
simulator internals, see the companion repository:
[`niobium-fhetch`](https://github.com/NiobiumInc/niobium-fhetch).

## Documentation

This README is the quickest way in; deeper references live under
[`docs/`](docs/):

- [`docs/index.md`](docs/index.md) — documentation landing page and map.
- [`docs/architecture.md`](docs/architecture.md) — the record-and-replay
  design, the API / core / common layering, and the replay-bridge boundary.
- [`docs/building.md`](docs/building.md) — standalone, submodule, and nix
  build flows in more depth than the Building section below.
- [`docs/testing.md`](docs/testing.md) — the Catch2 suites plus the benchmark
  and coverage tooling, and how to run each locally.
- [`docs/decision-log.md`](docs/decision-log.md) — the rationale behind every
  non-trivial design decision. It is the single source of truth for why a
  choice was made; code comments deliberately do not carry that rationale.
- [`docs/observability/README.md`](docs/observability/README.md) — the
  library-context observability model and the shipped dashboard template.

Runnable, copy-pasteable code lives under [`examples/`](examples/): the pure-C
[`quickstart.c`](examples/quickstart.c) and the decrypt-verified CKKS
[`ckks22.cpp`](examples/ckks22.cpp), with build and run instructions in
[`examples/README.md`](examples/README.md). Both files are byte-for-byte
mirrors of the two fenced examples in this README, which remain the source of
truth that CI extracts and gates.

## How it fits together

```
 Customer C / C++ Application
 (uses CUDA-shaped haze API: hazeMalloc, hazeAdd, hazeNTT, ...)
         |
         | #include <haze/haze.h>; links against libhaze
         v
 +--------------------------------------------------+
 |  libhaze  (this repo)                            |
 |  haze.h   — CUDA-shaped C entry points           |
 |  every call becomes one FHETCH Polynomial IR op  |
 +--------------------------------------------------+
         |
         | delegates recording to
         v
 +--------------------------------------------------+
 |  niobium-fhetch (libnbfhetch)                    |
 |   compiler() singleton + fhetch::* recording API |
 +--------------------------------------------------+
         |
         | hazeFlush() finalises the .fhetch trace + manifest
         | and dispatches replay; D2H then reads the result
         v
         +-------------------+--------------------+
         |                                        |
         v                                        v
 +-----------------------+            +----------------------+
 | local (default)       |            | Niobium Server       |
 | — in-process FHETCH   |            | (niobium-compiler)   |
 |   simulator inside    |            | — proprietary        |
 |   libnbfhetch         |            | — optimizes and      |
 | — populates D2H       |            |   deploys to Mistic, |
 |   reads with sim'd    |            |   FUNC_SIM, FHE_SIM, |
 |   ciphertext values   |            |   FPGA_TRI, ...      |
 +-----------------------+            +----------------------+
```

### Step by step

1. **Compile & Link** — The application includes `<haze/haze.h>` and links
   against `libhaze`. No probes, no instrumented OpenFHE. Haze pulls
   `libnbfhetch` in transitively for the recording back-end.

2. **Configure** — `hazeSetRingDimension(N)`, `hazeSetCiphertextModulus(idx, q)`
   for each residue, and `hazeConfigureDevice()` lock in the FHE parameter set.
   Optional: `hazeSetTarget("FUNC_SIM")` to pick a non-default replay target.
   (`libhaze` itself does not read `HAZE_TARGET`; that variable is honored only
   by the test and example harnesses, which bridge it to `hazeSetTarget`.)

3. **Allocate & Record** — `hazeMalloc` returns one FHETCH-addressable
   polynomial. Compute calls (`hazeAdd`, `hazeMul`, `hazeNTT`, `hazeAutomorph`,
   `hazeBasisConvert`, ...) record one or more FHETCH instructions per call;
   nothing executes yet. Stream and event handles are accepted for CUDA-shape
   parity but are intentionally no-ops — recording has no notion of
   stream-relative ordering until the trace is flushed.

4. **Declare, Flush & Read** — `hazeTagOutput(dev_ptr)` declares each result you
   want back, then `hazeFlush()` finalises the current epoch's `.fhetch` trace,
   dispatches it to the configured target, and populates the tagged outputs'
   shadow buffers. `hazeMemcpy(host_buf, dev_ptr, n, HAZE_MEMCPY_DEVICE_TO_HOST)`
   then reads them. `hazeFlush` is the sole flush trigger (`hazeDeviceSynchronize`
   is a no-op); a D2H of an untagged/unflushed address returns `HAZE_ERROR_NOT_FLUSHED`.

5. **Submit (production)** — Choose a compiler-side target such as `FPGA_TRI`
   (or `FUNC_SIM` / `FHE_SIM` / `fhetch_sim`) before replay. Haze's transport
   layer ships the recorded trace plus serialized inputs to a running
   `nbcc_fhetch_replay` instance, which exercises the Niobium compilation
   pipeline and either simulates or executes on real hardware.

## Recording an FHE op

Two runnable examples show the same libhaze operation — a 22-limb ciphertext
add — first in pure C, then in C++ wrapped in real CKKS. Both configure a
22-limb RNS modulus chain at ring dimension `N = 65536` and run the identical
C-ABI sequence (configure -> H2D -> `hazeAddMrp` over the base -> tag -> flush
-> D2H) through the in-process simulator; they differ only in the surrounding
crypto. Both are extracted and gated in CI (`make test-readme`), so the code
below cannot silently rot: `scripts/test_readme_examples.sh` compiles and runs
each one against the shipped `libhaze`.

The two fenced blocks below are the authoritative source of truth: CI extracts
them verbatim by their `readme-example` markers. The runnable copies under
[`examples/`](examples/) — `examples/quickstart.c` and `examples/ckks22.cpp` —
are byte-for-byte mirrors of these regions, so edit the README block, not the
mirror.

### C — raw 22-limb add, verified per residue

The C example links only `libhaze` (OpenFHE has no C API, so a `.c` translation
unit cannot encrypt or decrypt). It adds two polynomials across all 22 residues
in a single `hazeAddMrp` call over the base, then checks the recorded modular
sums directly: `1 + 2 == 3` in every coefficient of every limb.

<!-- readme-example:begin lang=c name=quickstart -->
```c
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* 22-limb RNS modulus chain at ring dimension N = 65536, matching a CKKS
   ciphertext. Each limb is a distinct NTT-friendly ~60-bit prime (q = 1 mod 2N). */
enum { kNumLimbs = 22 };

/* Fail the example loudly (jumping to main's single cleanup path) the moment a
   Haze call reports a non-success status, so a broken snippet cannot silently
   continue on null/invalid state and still "pass" the docs-as-tests check. */
#define HAZE_CHECK(expr)                                                                           \
    do {                                                                                           \
        const hazeError_t rc_ = (expr);                                                            \
        if (rc_ != HAZE_SUCCESS) {                                                                 \
            fprintf(stderr, "%s failed: %s (%d)\n", #expr, hazeGetErrorString(rc_), (int)rc_);     \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)

int main(void) {
    const uint64_t ring_dim = 65536;
    const size_t bytes = ring_dim * sizeof(uint64_t);

    static const uint64_t q[kNumLimbs] = {
        576460752308273153ULL, 576460752315482113ULL, 576460752319021057ULL, 576460752319414273ULL,
        576460752321642497ULL, 576460752325705729ULL, 576460752328327169ULL, 576460752329113601ULL,
        576460752329506817ULL, 576460752329900033ULL, 576460752331210753ULL, 576460752337502209ULL,
        576460752340123649ULL, 576460752342876161ULL, 576460752347201537ULL, 576460752347332609ULL,
        576460752352837633ULL, 576460752354017281ULL, 576460752355065857ULL, 576460752355459073ULL,
        576460752358604801ULL, 576460752364240897ULL,
    };

    /* Resources are declared and zero-initialized up front so the single
       cleanup path can release exactly what was acquired, even on early exit. */
    int status = 1;
    uint64_t *a = NULL;
    uint64_t *b = NULL;
    uint64_t *result = NULL;
    void *d_a[kNumLimbs] = {0};
    void *d_b[kNumLimbs] = {0};
    void *d_dst[kNumLimbs] = {0};
    const void *h_a[kNumLimbs];
    const void *h_b[kNumLimbs];
    void *h_res[kNumLimbs];
    uint64_t picked = 0;

    /* ---- Configure the FHE parameter set. ---- */
    HAZE_CHECK(hazeSetRingDimension(ring_dim));
    /* Seed the CryptoContext the local simulator uses to reconstruct values. */
    HAZE_CHECK(hazeReplayBridgeInitCryptoContext(ring_dim, q[0], &picked));
    for (int i = 0; i < kNumLimbs; ++i)
        HAZE_CHECK(hazeSetCiphertextModulus(i, q[i]));
    HAZE_CHECK(hazeConfigureDevice());

    /* ---- Allocate the MRP groups; stage host inputs (a = 1, b = 2). ---- */
    a = malloc(bytes);
    b = malloc(bytes);
    result = malloc((size_t)kNumLimbs * bytes);
    if (a == NULL || b == NULL || result == NULL) {
        fprintf(stderr, "host allocation failed\n");
        goto cleanup;
    }
    for (uint64_t i = 0; i < ring_dim; ++i) {
        a[i] = 1;
        b[i] = 2;
    }

    HAZE_CHECK(hazeMallocMrp(d_a, kNumLimbs, bytes));
    HAZE_CHECK(hazeMallocMrp(d_b, kNumLimbs, bytes));
    HAZE_CHECK(hazeMallocMrp(d_dst, kNumLimbs, bytes));
    for (int i = 0; i < kNumLimbs; ++i) {
        h_a[i] = a;
        h_b[i] = b;
        h_res[i] = result + (size_t)i * ring_dim;
    }

    /* ---- Stage the inputs, record the add, read the results: one MRP op
           each over the whole 22-limb base. ---- */
    HAZE_CHECK(hazeMemcpyMrp(d_a, h_a, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, q, kNumLimbs));
    HAZE_CHECK(hazeMemcpyMrp(d_b, h_b, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, q, kNumLimbs));
    HAZE_CHECK(hazeAddMrp(d_dst, (const void *const *)d_a, (const void *const *)d_b, q, kNumLimbs,
                          /*stream=*/NULL));

    HAZE_CHECK(hazeTagOutput(d_dst[0])); /* tagging one residue tags the whole group */
    HAZE_CHECK(hazeFlush());
    HAZE_CHECK(hazeMemcpyMrp(h_res, (const void *const *)d_dst, bytes, HAZE_MEMCPY_DEVICE_TO_HOST,
                             q, kNumLimbs));

    status = 0;
    for (int i = 0; i < kNumLimbs && status == 0; ++i) {
        const uint64_t *r = (const uint64_t *)h_res[i];
        for (uint64_t k = 0; k < ring_dim; ++k)
            if (r[k] != 3) { /* (1 + 2) mod q_i == 3 for every prime */
                printf("limb %d coeff %llu = %llu (expected 3)\n", i, (unsigned long long)k,
                       (unsigned long long)r[k]);
                status = 1;
                break;
            }
    }

cleanup:
    /* Deterministic teardown: release only the device groups that were
       allocated (a zero first handle means the group was never created), and
       surface any free failure in the exit status. */
    if (d_dst[0] != NULL && hazeFreeMrp(d_dst, kNumLimbs) != HAZE_SUCCESS)
        status = 1;
    if (d_b[0] != NULL && hazeFreeMrp(d_b, kNumLimbs) != HAZE_SUCCESS)
        status = 1;
    if (d_a[0] != NULL && hazeFreeMrp(d_a, kNumLimbs) != HAZE_SUCCESS)
        status = 1;
    free(result);
    free(a);
    free(b);

    if (status == 0)
        printf("readme-c: OK\n");
    return status;
}
```
<!-- readme-example:end -->

### C++ — the same add over a real CKKS ciphertext, decrypt-verified

The C++ example is the realistic capstone: a genuine CKKS ciphertext add. A
consumer's own stock OpenFHE handles `keygen` / `encrypt` / `decrypt`; libhaze
performs the compute. OpenFHE derives the 22-limb chain at `N = 65536`; haze is
configured from the moduli read off that context, both ciphertexts' `(c0, c1)`
polynomials are added with one `hazeAddMrp` per polynomial over the 22-limb
base, and the haze-computed result is injected back into an OpenFHE ciphertext
shell and decrypted to confirm the slots equal `x1 + x2`.

<!-- readme-example:begin lang=cpp name=ckks22 -->
```cpp
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <openfhe.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lbcrypto;

namespace {

// Turn any non-success Haze status into an exception so the single catch in
// main() converts a failed call into a clean non-zero exit instead of letting
// the example continue on null/invalid state and still print its OK token.
void haze_check(hazeError_t rc, const char *what) {
    if (rc != HAZE_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed: " + hazeGetErrorString(rc) + " (" +
                                 std::to_string(static_cast<int>(rc)) + ")");
}

// RAII owner of one MRP device group. The destructor frees the group, so every
// allocation is released deterministically as the scope unwinds -- including
// when a later checked call throws. Move-only; a moved-from group frees nothing.
class MrpGroup {
  public:
    MrpGroup() = default;
    explicit MrpGroup(std::size_t count) : ptrs_(count, nullptr) {}
    MrpGroup(MrpGroup &&other) noexcept : ptrs_(std::move(other.ptrs_)) { other.ptrs_.clear(); }
    MrpGroup &operator=(MrpGroup &&other) noexcept {
        if (this != &other) {
            free_now();
            ptrs_ = std::move(other.ptrs_);
            other.ptrs_.clear();
        }
        return *this;
    }
    MrpGroup(const MrpGroup &) = delete;
    MrpGroup &operator=(const MrpGroup &) = delete;
    ~MrpGroup() { free_now(); }

    void **data() { return ptrs_.data(); }
    std::size_t size() const { return ptrs_.size(); }
    void *operator[](std::size_t i) const { return ptrs_[i]; }

  private:
    // Best-effort release invoked from the destructor: never throws, frees only
    // a group that was actually allocated, and reports a failed free.
    void free_now() noexcept {
        if (!ptrs_.empty() && ptrs_[0] != nullptr)
            if (hazeFreeMrp(ptrs_.data(), ptrs_.size()) != HAZE_SUCCESS)
                std::fprintf(stderr, "warning: hazeFreeMrp failed during cleanup\n");
        ptrs_.clear();
    }

    std::vector<void *> ptrs_;
};

// Per-tower uint64 limbs of one ciphertext polynomial (c0 or c1).
std::vector<std::vector<uint64_t>> extract_chain(const DCRTPoly &poly, uint64_t n) {
    const std::size_t towers = poly.GetNumOfElements();
    std::vector<std::vector<uint64_t>> chain(towers, std::vector<uint64_t>(n));
    for (std::size_t t = 0; t < towers; ++t) {
        const auto &vals = poly.GetElementAtIndex(static_cast<uint32_t>(t)).GetValues();
        for (uint64_t i = 0; i < n; ++i)
            chain[t][i] = vals[i].ConvertToInt<uint64_t>();
    }
    return chain;
}

} // namespace

int main() {
    try {
        const auto t_start = std::chrono::steady_clock::now();

        // ---- Build a 22-limb CKKS context with stock OpenFHE. ----
        CCParams<CryptoContextCKKSRNS> params;
        params.SetMultiplicativeDepth(21); // 22 RNS towers (depth + 1)
        params.SetScalingModSize(55);
        params.SetFirstModSize(60);
        params.SetScalingTechnique(FIXEDAUTO); // tower count is exactly depth + 1
        params.SetSecurityLevel(HEStd_128_classic);
        params.SetBatchSize(8);
        auto cc = GenCryptoContext(params);
        cc->Enable(PKE);
        cc->Enable(KEYSWITCH);
        cc->Enable(LEVELEDSHE);
        auto keys = cc->KeyGen();

        const uint64_t N = cc->GetRingDimension();
        const std::size_t bytes = static_cast<std::size_t>(N) * sizeof(uint64_t);
        std::vector<uint64_t> q_base;
        for (const auto &p : cc->GetCryptoParameters()->GetElementParams()->GetParams())
            q_base.push_back(p->GetModulus().ConvertToInt<uint64_t>());
        const std::size_t towers = q_base.size();
        if (N != 65536 || towers != 22) {
            std::fprintf(stderr, "unexpected chain: N=%llu towers=%zu\n", (unsigned long long)N,
                         towers);
            return 1;
        }

        // ---- Encrypt two packed real-valued vectors. ----
        const std::vector<double> x1 = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
        const std::vector<double> x2 = {1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
        auto ct1 = cc->Encrypt(keys.publicKey, cc->MakeCKKSPackedPlaintext(x1));
        auto ct2 = cc->Encrypt(keys.publicKey, cc->MakeCKKSPackedPlaintext(x2));

        // ---- Configure haze for the same chain (moduli read off the context). ----
        haze_check(hazeDeviceReset(), "hazeDeviceReset");
        haze_check(hazeSetRingDimension(N), "hazeSetRingDimension");
        uint64_t picked = 0;
        haze_check(hazeReplayBridgeInitCryptoContext(N, q_base[0], &picked),
                   "hazeReplayBridgeInitCryptoContext");
        for (std::size_t i = 0; i < towers; ++i)
            haze_check(hazeSetCiphertextModulus(static_cast<int>(i), q_base[i]),
                       "hazeSetCiphertextModulus");
        haze_check(hazeConfigureDevice(), "hazeConfigureDevice");

        // ---- Device phase in an inner scope: the MRP groups free themselves
        //      (RAII) at the end of the block, before the host-only decrypt. ----
        std::vector<std::vector<uint64_t>> res0(towers, std::vector<uint64_t>(N));
        std::vector<std::vector<uint64_t>> res1(towers, std::vector<uint64_t>(N));
        {
            // Stage one ciphertext polynomial's limbs to the device as an MRP group.
            auto h2d = [&](const std::vector<std::vector<uint64_t>> &chain) {
                MrpGroup group(chain.size());
                haze_check(hazeMallocMrp(group.data(), group.size(), bytes), "hazeMallocMrp(h2d)");
                std::vector<const void *> src(chain.size(), nullptr);
                for (std::size_t t = 0; t < chain.size(); ++t)
                    src[t] = chain[t].data();
                haze_check(hazeMemcpyMrp(group.data(), src.data(), bytes,
                                         HAZE_MEMCPY_HOST_TO_DEVICE, q_base.data(), chain.size()),
                           "hazeMemcpyMrp(h2d)");
                return group;
            };
            MrpGroup a0 = h2d(extract_chain(ct1->GetElements()[0], N));
            MrpGroup a1 = h2d(extract_chain(ct1->GetElements()[1], N));
            MrpGroup b0 = h2d(extract_chain(ct2->GetElements()[0], N));
            MrpGroup b1 = h2d(extract_chain(ct2->GetElements()[1], N));

            MrpGroup r0(towers);
            MrpGroup r1(towers);
            haze_check(hazeMallocMrp(r0.data(), r0.size(), bytes), "hazeMallocMrp(r0)");
            haze_check(hazeMallocMrp(r1.data(), r1.size(), bytes), "hazeMallocMrp(r1)");

            // ---- Record the homomorphic add: one MRP op over the whole 22-limb
            //      base per ciphertext polynomial (c0, c1). ----
            haze_check(hazeAddMrp(r0.data(), a0.data(), b0.data(), q_base.data(), towers, nullptr),
                       "hazeAddMrp(r0)");
            haze_check(hazeAddMrp(r1.data(), a1.data(), b1.data(), q_base.data(), towers, nullptr),
                       "hazeAddMrp(r1)");
            haze_check(hazeTagOutput(r0[0]), "hazeTagOutput(r0)"); // tags the whole group
            haze_check(hazeTagOutput(r1[0]), "hazeTagOutput(r1)");
            haze_check(hazeFlush(), "hazeFlush");

            // ---- Read both result polynomials back as whole MRP groups. ----
            std::vector<void *> res0_ptrs(towers);
            std::vector<void *> res1_ptrs(towers);
            for (std::size_t t = 0; t < towers; ++t) {
                res0_ptrs[t] = res0[t].data();
                res1_ptrs[t] = res1[t].data();
            }
            haze_check(hazeMemcpyMrp(res0_ptrs.data(), r0.data(), bytes, HAZE_MEMCPY_DEVICE_TO_HOST,
                                     q_base.data(), towers),
                       "hazeMemcpyMrp(res0)");
            haze_check(hazeMemcpyMrp(res1_ptrs.data(), r1.data(), bytes, HAZE_MEMCPY_DEVICE_TO_HOST,
                                     q_base.data(), towers),
                       "hazeMemcpyMrp(res1)");
            // a0, a1, b0, b1, r0, r1 are released here by the MrpGroup destructors.
        }

        // ---- Inject the limbs into a shell of the right shape and decrypt. The
        //      shell is ct1 (level 0, 22 towers, scale 1 -- same shape as the sum),
        //      a non-answer: a no-op inject would decrypt to x1, not x1 + x2. ----
        auto shell = ct1->Clone();
        auto inject = [&](std::size_t elem, const std::vector<std::vector<uint64_t>> &rows) {
            auto &towers_vec = shell->GetElements()[elem].GetAllElements();
            for (std::size_t t = 0; t < towers; ++t) {
                auto &np = towers_vec[t];
                NativeVector nv(static_cast<uint32_t>(N), NativeInteger(np.GetModulus()));
                for (uint64_t i = 0; i < N; ++i)
                    nv[i] = NativeInteger(rows[t][i]);
                np.SetValues(nv, np.GetFormat());
            }
        };
        inject(0, res0);
        inject(1, res1);

        Plaintext out;
        cc->Decrypt(keys.secretKey, shell, &out);
        out->SetLength(x1.size());
        const auto slots = out->GetRealPackedValue();

        for (std::size_t i = 0; i < x1.size(); ++i) {
            const double err = std::fabs(slots[i] - (x1[i] + x2[i]));
            if (!(err <= 1e-6)) { // negated compare so a NaN slot (corrupt decrypt) also fails
                std::fprintf(stderr, "slot %zu = %.6f, want %.6f\n", i, slots[i], x1[i] + x2[i]);
                return 1;
            }
        }

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        std::printf("readme-cpp: OK (%.2f s)\n", elapsed);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
```
<!-- readme-example:end -->

Replace `hazeAddMrp` with any sequence of `hazeMulMrp`, `hazeNTTMrp`,
`hazeAutomorphMrp`, `hazeBasisConvert`, ... and the recording layer captures
every op in execution order. `test/test_compute.cpp` and
`test/test_basis_convert.cpp` exercise the full surface; the `test/e2e/` suite
builds the CKKS operation set (add, mult + relin, rotate, rescale) on the same
C ABI.

## Building

### Standalone (non-nix)

```sh
git submodule update --init --recursive   # or: make sync
make build MODE=release                   # Release into build/
make build MODE=debug                     # Debug   into dbuild/ (the default)
```

`MODE` defaults to `debug`, so a bare `make build` is equivalent to
`make build MODE=debug`. The same `MODE=` selector applies to every target
that produces or consumes build artefacts (`config`, `build`, `test`,
`test-unit`, `test-sim`, `test-transport`, `test-all`, `clean`).

The top-level `Makefile` builds OpenFHE (vendored at
`vendor/niobium-fhetch/vendor/openfhe`), installs it under
`vendor/niobium-fhetch/vendor/lib/openfhe`, then builds `libhaze` and
`haze_tests` against `Niobium::fhetch`. The first build is slow because OpenFHE
is compiled from source; subsequent invocations skip it.

To skip the OpenFHE build entirely (e.g. when a parent project already installed
it):

```sh
EXTERNAL_OPENFHE=1 OPENFHE_INSTALL_DIR=/path/to/openfhe make build MODE=release
```

#### Prerequisites

- clang >= 19 (C++23 required by `CMakeLists.txt`).
- cmake >= 3.22.
- Catch2 3.x.
- git (submodule init).

Clang 19 is the supported floor. The nix flake (and therefore CI) tracks
nixpkgs-unstable's default, which is currently newer; pick the most recent
clang available from your package manager when possible.

```sh
# macOS (Homebrew) — `llvm` is unversioned and tracks Homebrew's current
# release; pin to `llvm@19` only if you need a specific version.
brew install cmake catch2 llvm

# Debian / Ubuntu (Catch2 3.x may need a backport or source build) — bump
# clang-19 / llvm-19-dev to a newer apt suffix where available.
sudo apt install cmake catch2 clang-19 llvm-19-dev
```

#### Make targets

```
Build:
  sync              Init vendor/niobium-fhetch (recursive).
  config            Configure haze (uses MODE).
  build             Build haze (uses MODE).
  config-openfhe    Configure OpenFHE.
  build-openfhe     Build and install OpenFHE locally.

Test:
  test-unit         Unit suite (HAZE_TARGET=local; no FHE math).
  test-sim          Sim suite via the in-process FHETCH simulator
                    (HAZE_TARGET=local). Validates FHE math.
  test-e2e          E2E suite (public C ABI + stock OpenFHE, decrypt).
  test-readme       Compile + run the README examples (C + C++).
  test-transport    [integration] suite via nbcc_fhetch_replay
                    (opt-in; requires NIOBIUM_COMPILER_ROOT).
  test-isolation    Assert libhaze exports only the haze* C ABI.
  test              Default: test-unit + test-sim + test-e2e + test-isolation.
  test-all          test + test-readme + test-transport.

Cleanup:
  clean-runs        Remove test runs/ artifacts.
  clean             Remove all build artifacts (refuses to touch
                    external trees pointed at via
                    NIOBIUM_HAZE_FHETCH_DIR or EXTERNAL_OPENFHE).
```

`make help` prints the same list at runtime.

#### Override knobs

Make variables and / or environment:

| Variable                  | Purpose                                                                                                     | Default                                   |
| ------------------------- | ----------------------------------------------------------------------------------------------------------- | ----------------------------------------- |
| `MODE`                    | `debug` or `release`. Selects `dbuild`/`build` and CMake `Debug`/`Release`.                                 | `debug`                                   |
| `NUM_CPUS`                | Build parallelism.                                                                                          | Auto (`sysctl -n hw.ncpu` / `nproc`).     |
| `NIOBIUM_HAZE_FHETCH_DIR` | External `niobium-fhetch` source tree to use instead of `vendor/niobium-fhetch`.                            | unset (vendor submodule).                 |
| `OPENFHE_INSTALL_DIR`     | Where OpenFHE is installed (libs + headers).                                                                | `<fhetch>/vendor/lib/openfhe`.            |
| `EXTERNAL_OPENFHE`        | `1` skips the OpenFHE build chain (caller supplies it via `OPENFHE_INSTALL_DIR`).                           | `0`.                                      |
| `JSON_INCLUDE_DIR`        | `nlohmann/json` single-include directory.                                                                   | unset (use niobium-fhetch's vendor copy). |
| `NIOBIUM_COMPILER_ROOT`   | Path to a `niobium-compiler` checkout containing `build/nbcc_fhetch_replay`. Required for `test-transport`. | unset.                                    |

Runtime target selector (honored by the test and example harness, **not** by `libhaze` itself and not by the Makefile):

| Variable      | Purpose                                                                                                                                                                                                                                                     |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `HAZE_TARGET` | Replay target for the test/example harness: `local` (default; in-process simulator) or one of `FHE_SIM`, `FUNC_SIM`, `FPGA_TRI`, `fhetch_sim` (HTTP transport to `nbcc_fhetch_replay`). `libhaze` does **not** read this variable; the harness forwards it to `hazeSetTarget()` before the first `hazeFlush()`. Application code must call `hazeSetTarget()` explicitly. See [`include/haze/haze.h`](include/haze/haze.h) for the full target table. |

CMake-level toggles (`-D...`):

| Option                                     | Default | Effect                                                               |
| ------------------------------------------ | ------- | -------------------------------------------------------------------- |
| `HAZE_SANITIZERS`                          | `OFF`   | ASAN + UBSAN.                                                        |
| `HAZE_TSAN`                                | `OFF`   | TSAN. Mutually exclusive with `HAZE_SANITIZERS`.                     |
| `HAZE_FBC_REDUCED_NOISE`                   | `ON`    | Test oracle uses OpenFHE's `ReducedNoise` FBC variant.               |
| `NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS` | `OFF`   | Register `haze_transport_tests` as a ctest entry (parent-build use). |

### As a `niobium-client` submodule

When haze is checked out under `niobium-client/vendor/niobium-haze`, the parent
owns the build graph and haze's own `Makefile` is not invoked. The parent
exposes wrapper targets in the `##@ Haze` section of its top-level `Makefile`.

Building (from the niobium-client root):

```sh
make build-release          # full client build; produces
                            # build/vendor/niobium-haze/{haze_tests,libhaze.*,
                            # libhaze_replay_bridge.*} via the parent CMake
                            # graph. NIOBIUM_CLIENT_WITH_HAZE=ON is the default.
make build-haze-release     # rebuild only the haze targets without touching
                            # the rest of the client (handy while iterating).
```

Testing (from the niobium-client root):

```sh
make test-haze-unit-release
# Same coverage as standalone `make test-unit`. Runs
# haze_tests "~[integration]" with HAZE_TARGET=local. No transport,
# no compiler binary required.

make test-haze-integration-release \
    NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
# End-to-end transport round trip:
#   1. spawns build/src/fhetch_transport/nbcc_fhetch_replay_server,
#   2. puts the client-side forwarder first on PATH,
#   3. runs haze_tests "[integration]" with HAZE_TARGET=FUNC_SIM.
# Differs from standalone `make test-transport`, which dispatches to
# nbcc_fhetch_replay directly without the in-tree forwarder/server pair.

make test-haze-release      # both of the above.
make clean-haze             # drop build/vendor/niobium-haze/runs/ only.
```

Path differences vs the standalone build:

| Artifact      | Parent-driven                          | Standalone         |
| ------------- | -------------------------------------- | ------------------ |
| `haze_tests`  | `build/vendor/niobium-haze/haze_tests` | `build/haze_tests` |
| Test runs dir | `build/vendor/niobium-haze/runs/`      | `build/runs/`      |

### With the nix flake (optional)

The standalone Makefile flow above is the **first-class** path; the flake is
an opt-in convenience for contributors who already use nix. It provides three
distinct surfaces.

#### 1. Dev shell — fastest iteration

```sh
nix develop                                    # interactive
nix develop --command make build               # one-shot
```

The shell provisions the toolchain (clang as `cc`/`c++`, cmake, Catch2 3,
clang-tools, jujutsu, nixfmt) but otherwise stays out of the way. From there
the Makefile flow above (`make build`, `make test`, etc.) works unchanged.
This is the recommended path for editing haze sources — every iteration uses
the live worktree without round-tripping through `/nix/store`.

#### 2. `nix run` apps — make targets without entering the shell

```sh
nix run .#test-unit                # = nix develop --command make test-unit
nix run .#test-sim                 # = nix develop --command make test-sim
nix run .#test                     # = ... make test
nix run .#build                    # = ... make build
```

Each app re-enters the dev shell so cmake's setup hooks fire, then invokes
the Makefile target against the caller's CWD (which must be a haze worktree).

#### 3. Hermetic packages — cached, reproducible, slow first time

```sh
nix build .#openfhe                # ~20-30 min cold; cached in /nix/store
nix build .#niobium-fhetch         # depends on openfhe; reuses cache
nix build .#haze                   # = .#default; libhaze + haze_tests
nix flake check                    # devshell + fmt + haze build + tests
```

Each layer caches independently. OpenFHE only rebuilds when its submodule
pointer moves; haze edits only invalidate the haze derivation. After
`nix build .#haze`, the result is at `result/{lib,bin,include}/...`.

The `nix flake check` `unit-tests` and `sim-tests` derivations run `haze_tests`
hermetically — no live worktree required, suitable for CI.

#### Notes

- The flake declares a dedicated `niobium-fhetch` input (external pin of
  `git+https://github.com/NiobiumInc/niobium-fhetch.git?submodules=1`)
  that only the hermetic `mkPackages` derivations consume; the dev shell
  never references it. Lazy fetching means `nix develop` evaluates without
  touching the fhetch remote, so a clean haze worktree drops straight into
  the dev shell without network access. `nix build .#haze` / `nix flake check` /
  `nix flake update` still touch the input and need network access to
  github.com on a clean worktree (nix issue #13324); the repo is public, so
  HTTPS fetches work without SSH credentials. The submodule under `vendor/niobium-fhetch` remains
  the source of truth for `make build` (non-nix users); CI gates that the
  submodule rev recorded in haze's index matches the rev pinned in
  `flake.lock`. After bumping the submodule, run
  `scripts/sync-fhetch-rev.sh` to realign `flake.lock` and commit both
  in the same change.
- The hermetic packages live entirely in `/nix/store`. The macOS SDK / ABI
  mismatch trap (see [`CLAUDE.md`](CLAUDE.md)) only triggers when **mixing**
  nix and non-nix builds in the same closure — pure-flake or pure-Makefile
  workflows are unaffected. Don't `make build` against an OpenFHE that was
  previously built inside `nix develop` (or vice versa).
- Invoke from a parent repo with the explicit path: `nix run
  path:./vendor/niobium-haze#test-unit`. The apps embed the haze flake's
  self-reference, so they load the haze dev shell regardless of CWD.

## Editor integration (Zed)

`.zed/settings.json` and `.envrc` are checked in. To get accurate
diagnostics:

```sh
direnv allow      # one-time, loads the nix devshell
make build        # populates build/compile_commands.json
zed .
```

If `.clang-tidy` or `.clangd` change, restart clangd via Zed's command
palette → `editor: restart language server`.

## Testing

Three suites, all built into a single `haze_tests` Catch2 binary and split by
tag plus environment:

- `test-unit` — state-machine and recording-only coverage. Runs every case not
  tagged `[integration]`. Does not perform a D2H and does not validate FHE
  math results. Fast (~1 second).
- `test-sim` — `[integration]`-tagged cases run through libnbfhetch's
  in-process FHETCH simulator (`HAZE_TARGET=local`). Validates real FHE math
  without an external binary or HTTP transport.
- `test-transport` — same `[integration]` filter, but the recorded trace is
  shipped over HTTP to a `niobium-compiler`-built `nbcc_fhetch_replay`.
  Requires `NIOBIUM_COMPILER_ROOT` pointing at a checkout containing
  `build/nbcc_fhetch_replay`.

## Benchmarking

A [Google Benchmark](https://github.com/google/benchmark) micro-benchmark suite
lives under [`benchmark/`](benchmark/). It measures the polynomial-level FHE ops
— add, multiply, NTT, and automorph, in both their base (SRP) and MRP forms —
and the full record -> flush -> replay path through the in-process simulator.

```sh
make bench          # configures with -DHAZE_BUILD_BENCHMARKS=ON and runs the suite
```

The benchmarks build as a **separate executable** that links the compiled haze
objects; they are never absorbed into the shipped `libhaze`, so the library's
ABI and the symbol-leak audit are unaffected. A CI job runs the suite, emits
JSON, and compares it against the checked-in baseline at
[`benchmark/baseline.json`](benchmark/baseline.json), flagging regressions
beyond a tolerance. The suite establishes regression detection only; it does not
tune the ops or the record/replay path.

## Coverage

> **Status: delivered.** The `HAZE_COVERAGE` CMake option, the `make coverage`
> target, the coverage driver [`scripts/coverage.sh`](scripts/coverage.sh), and
> the [`.github/workflows/coverage.yml`](.github/workflows/coverage.yml) CI gate
> are all wired in this tree. `make coverage` builds the instrumented tree, runs
> the suite, and enforces the **80% line-coverage threshold** on
> [`src/core/`](src/core/) and [`src/api/`](src/api/) — currently passing at
> **85.85%**. The rationale for sequencing the gate after the backfill/hardening
> work is recorded in [`docs/decision-log.md`](docs/decision-log.md) (D-06, D-10).

Coverage uses Clang's source-based instrumentation. The `-DHAZE_COVERAGE=ON`
build option adds `-fprofile-instr-generate -fcoverage-mapping` (Clang only);
the coverage driver [`scripts/coverage.sh`](scripts/coverage.sh) runs the
instrumented test binary, merges the raw profiles with `llvm-profdata merge`,
and exports an lcov report with `llvm-cov export -format=lcov`, scoped to the
runtime sources under [`src/core/`](src/core/) and [`src/api/`](src/api/).

```sh
make coverage       # builds instrumented, runs the suite, enforces the 80% gate
```

The [`.github/workflows/coverage.yml`](.github/workflows/coverage.yml) job runs
the same flow per PR and fails below the threshold. The gate is intentionally
sequenced *after* the backfill and error-path hardening work so that enabling it
did not turn the branch red retroactively.

## Observability

Haze is a C++ shared library with a C ABI rather than a network service, so the
usual "structured logs + tracing + metrics endpoint + health checks" model is
reinterpreted for the library context (the reasoning is recorded in
[`docs/decision-log.md`](docs/decision-log.md)):

- **Structured logging with correlation IDs** — the tagged sink in
  [`src/common/log.hpp`](src/common/log.hpp) /
  [`src/common/log.cpp`](src/common/log.cpp) stamps each line
  (`[haze] [cid=<id>] <tag>: <body>`) with a correlation ID assigned by the
  recording epoch (streams are documented no-ops), so a single record ->
  flush -> replay cycle can be followed end to end. Lines are written with one
  line-atomic `fwrite`, control/non-ASCII bytes are escaped, and absolute paths
  are redacted to their basename to avoid leaking host layout.
- **Metrics surface** — the "metrics endpoint" is the `hazeGetPerformanceCounters`
  query surface, reporting op counts, bytes moved, and flush timings via the
  additive `hazePerformanceCounters` struct.
- **Tracing** — opt-in op / epoch spans (enabled via the `HAZE_TRACE`
  environment variable, re-read on each span so it can be toggled at runtime;
  **off by default**) trace the record -> flush -> replay path, including the
  crossings into the `replay_bridge/` OpenFHE boundary.
- **Health / readiness** — maps to lifecycle and config-state introspection
  (whether a device is configured, an epoch is open, and it has flushed).
- **Dashboard template** — a ready-to-import template ships under
  [`docs/observability/`](docs/observability/); see
  [`docs/observability/README.md`](docs/observability/README.md) for the
  counters-to-metrics mapping.

The three documented no-op entry points (`hazeStreamSynchronize`,
`hazeStreamWaitEvent`, `hazeDeviceSynchronize`) stay pure and are deliberately
left un-instrumented.

## Project structure

```
niobium-haze/
  include/haze/             # public C API (haze.h, haze_types.h)
  src/
    api/                    # extern-C boundary; one TU per public-header
                            # section (compute, memory, config, device,
                            # stream, lifecycle, error, basis_convert)
    core/                   # internal implementation: epoch, allocator,
                            # backend, config, basis convert, polynomial IO
    common/                 # shared leaf utilities (errors, log)
  replay_bridge/            # OpenFHE-using helper linked into libhaze;
                            # synthesises the cryptocontext + ciphertext
                            # templates the compiler-side replay needs
  test/                     # Catch2 3.x test suite
  scripts/                  # integration test harnesses
  vendor/
    niobium-fhetch/         # submodule: libnbfhetch + simulator + API
      vendor/openfhe/       # nested submodule
      vendor/json/          # nested submodule
  CMakeLists.txt            # libhaze + haze_tests + replay_bridge wiring
  Makefile                  # standalone driver (build, test-*, clean)
  flake.nix                 # nix dev shell + make-wrapping apps +
                            # hermetic packages (openfhe, niobium-fhetch, haze)
  CLAUDE.md                 # working notes (incl. macOS SDK trap recipe)
  README.md
```

## Architecture decisions

- **CUDA-like API by design** — Library authors targeting GPUs already think
  in `Malloc` / `Memcpy` / kernel calls. Haze preserves that shape so a CUDA
  FHE library can be retargeted to Niobium hardware by linking against `libhaze`
  instead of `libcudart`. Stream and event handles are accepted as intentional
  no-ops (they return success rather than being removed), so porting code does
  not need to delete every reference to `cudaStream_t`. Graph capture, once a
  no-op stub, is now implemented on top of the recording model (see the next
  point).

- **Graph capture, peer access, and performance counters** — Three CUDA-shaped
  surfaces that previously returned `HAZE_ERROR_NOT_SUPPORTED` now carry real
  behavior:
  - **Graph capture** — `hazeStreamBeginCapture` / `hazeStreamEndCapture` and
    `hazeGraphInstantiate` / `hazeGraphLaunch` / `hazeGraphExecUpdate` /
    `hazeGraphExecDestroy` / `hazeGraphDestroy` implement the CUDA
    record-once / replay-many contract: capture snapshots the recorded FHETCH
    op sequence, instantiate prepares a replayable exec, and each launch
    re-dispatches that snapshot with stable `DevAddr` operands.
  - **Peer access** — `hazeDeviceEnablePeerAccess`, `hazeDeviceCanAccessPeer`,
    and `hazeMemcpyPeerAsync` are implemented for the **simulator-representable**
    peer topology only. Physical multi-chip validation on real hardware is
    explicitly deferred as human follow-up and is not exercised by the default
    test suite.
  - **Performance counters** — `hazeGetPerformanceCounters` populates the
    additive `hazePerformanceCounters` struct with op counts, bytes moved, and
    flush timings.

  The three documented no-ops — `hazeStreamSynchronize`, `hazeStreamWaitEvent`,
  and `hazeDeviceSynchronize` — are unaffected and remain intentional no-ops by
  design.

- **Recording, not execution** — Every haze call appends to an in-memory
  FHETCH trace. Nothing executes until `hazeFlush()` dispatches the
  recording: it finalises the trace, runs replay, and populates the tagged
  outputs' shadow buffers, which a subsequent D2H reads. Stream-relative
  ordering is therefore not modelled; the
  recording is single-threaded by construction and the .fhetch trace
  itself defines the schedule the compiler optimises against.

- **Polynomial-level instead of OpenFHE-level** — `niobium-client` integrates
  at OpenFHE's `EvalAdd` / `EvalMult` boundary; haze sits one layer below at
  `NTT` / `INTT` / `Add` / `Mul` / `Automorph` / `BasisConvert`. Use haze when
  the caller wants explicit control over the polynomial schedule (e.g. porting
  a CUDA-resident FHE library); use `niobium-client` when the caller wants to
  run an unmodified OpenFHE app.

- **Replay bridge isolates OpenFHE** — `libhaze` itself is FHETCH-only and
  does not link OpenFHE into its public surface. `replay_bridge/` is a
  separately-built shared library that uses OpenFHE to synthesise the
  `cryptocontext.dat`, `ciphertext_templates/`, and per-input `.bin` / `.ids`
  artefacts the compiler-side `nbcc_fhetch_replay` needs to consume a haze
  recording. Splitting it out keeps OpenFHE includes out of `libhaze`'s
  translation units and out of downstream consumers.

- **Two replay tiers, one trace format** — The `local` target runs the same
  recorded `.fhetch` trace through the in-process simulator inside `libnbfhetch`
  (no external binary, no transport). The compiler-side targets (`FUNC_SIM`,
  `FHE_SIM`, `FPGA_TRI`, `fhetch_sim`) ship the same trace over HTTP to
  `nbcc_fhetch_replay`. Switching between them is a single `hazeSetTarget`
  call; the application code does not change. (The `HAZE_TARGET` environment
  variable selects the target only in the test/example harness, which forwards
  it to `hazeSetTarget`; `libhaze` itself does not read it.)

## License

See the `LICENSE` file.
