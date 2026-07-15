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
