#include <openfhe.h>

#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lbcrypto;

// Per-tower uint64 limbs of one ciphertext polynomial (c0 or c1).
static std::vector<std::vector<uint64_t>> extract_chain(const DCRTPoly &poly, uint64_t n) {
    const std::size_t towers = poly.GetNumOfElements();
    std::vector<std::vector<uint64_t>> chain(towers, std::vector<uint64_t>(n));
    for (std::size_t t = 0; t < towers; ++t) {
        const auto &vals = poly.GetElementAtIndex(static_cast<uint32_t>(t)).GetValues();
        for (uint64_t i = 0; i < n; ++i)
            chain[t][i] = vals[i].ConvertToInt<uint64_t>();
    }
    return chain;
}

int main() {
    const auto t_start = std::chrono::steady_clock::now();

    // ---- Build a 22-limb CKKS context with stock OpenFHE. ----
    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(21);     // 22 RNS towers (depth + 1)
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
    hazeDeviceReset();
    hazeSetRingDimension(N);
    uint64_t picked = 0;
    hazeReplayBridgeInitCryptoContext(N, q_base[0], &picked);
    for (std::size_t i = 0; i < towers; ++i)
        hazeSetCiphertextModulus(static_cast<int>(i), q_base[i]);
    hazeConfigureDevice();

    // ---- Stage each ciphertext's (c0, c1) limbs to the device as one MRP group. ----
    auto h2d = [&](const std::vector<std::vector<uint64_t>> &chain) {
        const std::size_t n = chain.size();
        std::vector<void *> ptrs(n, nullptr);
        hazeMallocMrp(ptrs.data(), n, bytes);
        std::vector<const void *> src(n, nullptr);
        for (std::size_t t = 0; t < n; ++t)
            src[t] = chain[t].data();
        hazeMemcpyMrp(ptrs.data(), src.data(), bytes, HAZE_MEMCPY_HOST_TO_DEVICE,
                      q_base.data(), n);
        return ptrs;
    };
    const auto a0 = h2d(extract_chain(ct1->GetElements()[0], N));
    const auto a1 = h2d(extract_chain(ct1->GetElements()[1], N));
    const auto b0 = h2d(extract_chain(ct2->GetElements()[0], N));
    const auto b1 = h2d(extract_chain(ct2->GetElements()[1], N));

    std::vector<void *> r0(towers, nullptr), r1(towers, nullptr);
    hazeMallocMrp(r0.data(), towers, bytes);
    hazeMallocMrp(r1.data(), towers, bytes);

    // ---- Record the homomorphic add: one MRP op over the whole 22-limb base
    //      per ciphertext polynomial (c0, c1). ----
    hazeAddMrp(r0.data(), a0.data(), b0.data(), q_base.data(), towers, nullptr);
    hazeAddMrp(r1.data(), a1.data(), b1.data(), q_base.data(), towers, nullptr);
    hazeTagOutput(r0[0]); // tagging one residue tags the whole group
    hazeTagOutput(r1[0]);
    hazeFlush();

    // ---- Read both result polynomials back as whole MRP groups (shadow reads). ----
    std::vector<std::vector<uint64_t>> res0(towers, std::vector<uint64_t>(N));
    std::vector<std::vector<uint64_t>> res1(towers, std::vector<uint64_t>(N));
    std::vector<void *> res0_ptrs(towers), res1_ptrs(towers);
    for (std::size_t t = 0; t < towers; ++t) {
        res0_ptrs[t] = res0[t].data();
        res1_ptrs[t] = res1[t].data();
    }
    hazeMemcpyMrp(res0_ptrs.data(), r0.data(), bytes, HAZE_MEMCPY_DEVICE_TO_HOST,
                  q_base.data(), towers);
    hazeMemcpyMrp(res1_ptrs.data(), r1.data(), bytes, HAZE_MEMCPY_DEVICE_TO_HOST,
                  q_base.data(), towers);

    // ---- Release the device groups; the rest is host-side OpenFHE. ----
    hazeFreeMrp(a0.data(), a0.size());
    hazeFreeMrp(a1.data(), a1.size());
    hazeFreeMrp(b0.data(), b0.size());
    hazeFreeMrp(b1.data(), b1.size());
    hazeFreeMrp(r0.data(), r0.size());
    hazeFreeMrp(r1.data(), r1.size());

    // ---- Inject the limbs into a shell of the right shape and decrypt. The
    //      shell is ct1 (level 0, 22 towers, scale 1 — same shape as the sum),
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
}
