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
