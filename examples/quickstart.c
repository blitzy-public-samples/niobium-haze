#include <haze/haze.h>
#include <haze/replay_bridge.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* 22-limb RNS modulus chain at ring dimension N = 65536, matching a CKKS
   ciphertext. Each limb is a distinct NTT-friendly ~60-bit prime (q = 1 mod 2N). */
enum { kNumLimbs = 22 };

int main(void) {
    const uint64_t ring_dim = 65536;
    const size_t   bytes    = ring_dim * sizeof(uint64_t);

    static const uint64_t q[kNumLimbs] = {
        576460752308273153ULL, 576460752315482113ULL, 576460752319021057ULL,
        576460752319414273ULL, 576460752321642497ULL, 576460752325705729ULL,
        576460752328327169ULL, 576460752329113601ULL, 576460752329506817ULL,
        576460752329900033ULL, 576460752331210753ULL, 576460752337502209ULL,
        576460752340123649ULL, 576460752342876161ULL, 576460752347201537ULL,
        576460752347332609ULL, 576460752352837633ULL, 576460752354017281ULL,
        576460752355065857ULL, 576460752355459073ULL, 576460752358604801ULL,
        576460752364240897ULL,
    };

    /* ---- Configure the FHE parameter set. ---- */
    hazeSetRingDimension(ring_dim);
    /* Seed the CryptoContext the local simulator uses to reconstruct values. */
    uint64_t picked = 0;
    hazeReplayBridgeInitCryptoContext(ring_dim, q[0], &picked);
    for (int i = 0; i < kNumLimbs; ++i)
        hazeSetCiphertextModulus(i, q[i]);
    hazeConfigureDevice();

    /* ---- Allocate the MRP groups; stage host inputs (a = 1, b = 2). ---- */
    void *d_a[kNumLimbs], *d_b[kNumLimbs], *d_dst[kNumLimbs];
    const void *h_a[kNumLimbs], *h_b[kNumLimbs];
    void *h_res[kNumLimbs];

    uint64_t *a = malloc(bytes), *b = malloc(bytes);
    uint64_t *result = malloc((size_t)kNumLimbs * bytes);
    if (a == NULL || b == NULL || result == NULL) return 2;
    for (uint64_t i = 0; i < ring_dim; ++i) { a[i] = 1; b[i] = 2; }

    hazeMallocMrp(d_a,   kNumLimbs, bytes);
    hazeMallocMrp(d_b,   kNumLimbs, bytes);
    hazeMallocMrp(d_dst, kNumLimbs, bytes);
    for (int i = 0; i < kNumLimbs; ++i) {
        h_a[i]   = a;
        h_b[i]   = b;
        h_res[i] = result + (size_t)i * ring_dim;
    }

    /* ---- Stage the inputs, record the add, read the results: one MRP op
           each over the whole 22-limb base. ---- */
    hazeMemcpyMrp(d_a, h_a, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, q, kNumLimbs);
    hazeMemcpyMrp(d_b, h_b, bytes, HAZE_MEMCPY_HOST_TO_DEVICE, q, kNumLimbs);
    hazeAddMrp(d_dst, (const void *const *)d_a, (const void *const *)d_b, q, kNumLimbs,
               /*stream=*/NULL);

    hazeTagOutput(d_dst[0]); /* tagging one residue tags the whole group */
    hazeFlush();
    hazeMemcpyMrp(h_res, (const void *const *)d_dst, bytes, HAZE_MEMCPY_DEVICE_TO_HOST, q,
                  kNumLimbs);

    int ok = 1;
    for (int i = 0; i < kNumLimbs && ok; ++i) {
        const uint64_t *r = (const uint64_t *)h_res[i];
        for (uint64_t k = 0; k < ring_dim; ++k)
            if (r[k] != 3) { /* (1 + 2) mod q_i == 3 for every prime */
                printf("limb %d coeff %llu = %llu (expected 3)\n", i,
                       (unsigned long long)k, (unsigned long long)r[k]);
                ok = 0;
                break;
            }
    }

    hazeFreeMrp(d_a,   kNumLimbs);
    hazeFreeMrp(d_b,   kNumLimbs);
    hazeFreeMrp(d_dst, kNumLimbs);
    free(a);
    free(b);
    free(result);

    if (!ok) return 1;
    printf("readme-c: OK\n");
    return 0;
}
