extern "C" {
    #include "compute.h"
}
#include <hip/hip_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define HIP_CHECK(call) \
    do { \
        hipError_t err = call; \
        if (err != hipSuccess) { \
            fprintf(stderr, "HIP Error: %s at %s:%d\n", hipGetErrorString(err), __FILE__, __LINE__); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

static thread_local unsigned long long *d_best_and_bid = nullptr;
static thread_local uint64_t           *d_indices = nullptr;
static thread_local uint8_t            *d_arrays = nullptr;
static thread_local int                allocated_blocks = 0;

__device__ __forceinline__ uint32_t rotl32(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

__device__ __forceinline__ uint64_t splitmix64(uint64_t *z) {
    *z += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t v = *z;
    v = (v ^ (v >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)) * UINT64_C(0x94d049bb133111eb);
    return v ^ (v >> 31);
}

__device__ __forceinline__ void make_rng(uint64_t seed, uint64_t index, uint32_t s[4]) {
    uint64_t z = seed + index * UINT64_C(0x9e3779b97f4a7c15);
    uint64_t a = splitmix64(&z);
    uint64_t b = splitmix64(&z);
    s[0] = (uint32_t)a;        s[1] = (uint32_t)(a >> 32);
    s[2] = (uint32_t)b;        s[3] = (uint32_t)(b >> 32);
    if (!s[0] && !s[1] && !s[2] && !s[3]) s[0] = 1;
}

__device__ __forceinline__ uint32_t rng_next(uint32_t s[4]) {
    const uint32_t result = rotl32(s[0] + s[3], 7) + s[0];
    const uint32_t t = s[1] << 9;
    s[2] ^= s[0]; s[3] ^= s[1];
    s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl32(s[3], 11);
    return result;
}

__device__ __forceinline__ uint32_t rng_bounded(uint32_t s[4], const uint32_t n) {
    const uint32_t threshold = (uint32_t)(((uint64_t)1 << 32) % (uint64_t)n);
    uint32_t val;
    do { val = rng_next(s); } while (val < threshold);
    return val % n;
}

__device__ __forceinline__ void warp_reduce(
    uint32_t &best_c, uint64_t &best_i, uint32_t &best_lane
) {
    #pragma unroll
    for (int off = 16; off > 0; off >>= 1) {
        uint32_t oc = __shfl_xor(best_c,    off, 32);
        uint64_t oi = __shfl_xor(best_i,    off, 32);
        uint32_t ol = __shfl_xor(best_lane, off, 32);
        if (oc > best_c) { best_c = oc; best_i = oi; best_lane = ol; }
    }
}

__global__ __launch_bounds__(256, 5)
void compute_kernel(
    uint64_t seed,
    uint64_t base_index,
    uint64_t total_work,
    unsigned long long *out_best_and_bid,
    uint64_t           *out_indices,
    uint8_t            *out_arrays
) {
    __shared__ uint32_t fys[25 * 256];
    uint32_t *arr = fys + threadIdx.x;

    const uint32_t tid    = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t stride = gridDim.x  * blockDim.x;
    const uint64_t end    = base_index + total_work;
    const uint32_t lane   = threadIdx.x & 31;
    const uint32_t warpid = threadIdx.x >> 5;

    uint32_t best_c = 0;
    uint64_t best_i = base_index + tid;
    uint8_t  best_arr[25];

    #pragma unroll
    for (int p = 0; p < 25; p++) best_arr[p] = (uint8_t)(p + 1);

    for (uint64_t idx = base_index + tid; idx < end; idx += stride) {
        uint32_t s[4];
        make_rng(seed, idx, s);

        #pragma unroll
        for (int p = 0; p < 25; p++) arr[p * 256] = (uint32_t)(p + 1);

        #pragma unroll
        for (int i = 24; i >= 1; i--) {
            const uint32_t j   = rng_bounded(s, (uint32_t)(i + 1));
            const uint32_t tmp = arr[i * 256];
            arr[i * 256] = arr[j * 256];
            arr[j * 256] = tmp;
        }

        uint32_t fp = 0;
        #pragma unroll
        for (int p = 0; p < 25; p++) fp += (arr[p * 256] == (uint32_t)(p + 1));

        if (fp > best_c) {
            best_c = fp;
            best_i = idx;
            #pragma unroll
            for (int p = 0; p < 25; p++) best_arr[p] = (uint8_t)arr[p * 256];
            if (fp == 25) break;
        }
    }

    __syncthreads();

    uint32_t best_lane = lane;
    warp_reduce(best_c, best_i, best_lane);

    uint8_t warp_arr[25];
    #pragma unroll
    for (int p = 0; p < 25; p++) {
        uint32_t v = __shfl((uint32_t)best_arr[p], best_lane, 32);
        if (lane == 0) warp_arr[p] = (uint8_t)v;
    }

    uint32_t *sh_c   = fys;
    uint64_t *sh_i   = (uint64_t *)(sh_c + 8);
    uint8_t  *sh_arr = (uint8_t  *)(sh_i + 8);

    if (lane == 0) {
        sh_c[warpid] = best_c;
        sh_i[warpid] = best_i;
        #pragma unroll
        for (int p = 0; p < 25; p++) sh_arr[warpid * 25 + p] = warp_arr[p];
    }
    __syncthreads();

    if (warpid != 0) return;

    best_c    = (lane < 8) ? sh_c[lane] : 0;
    best_i    = (lane < 8) ? sh_i[lane] : base_index;
    best_lane = lane;

    #pragma unroll
    for (int off = 4; off > 0; off >>= 1) {
        uint32_t oc = __shfl_xor(best_c,    off, 32);
        uint64_t oi = __shfl_xor(best_i,    off, 32);
        uint32_t ol = __shfl_xor(best_lane, off, 32);
        if (oc > best_c) { best_c = oc; best_i = oi; best_lane = ol; }
    }

    if (lane == 0) {
        const uint32_t blk = blockIdx.x;
        out_indices[blk] = best_i;

        #pragma unroll
        for (int p = 0; p < 25; p++) {
            out_arrays[blk * 25 + p] = sh_arr[best_lane * 25 + p];
        }

        atomicMax(out_best_and_bid, ((unsigned long long)best_c << 32) | (unsigned long long)blk);
    }
}

extern "C" compute_result compute_run(uint64_t seed, uint64_t lo, uint64_t hi) {
    compute_result best = { .best_correct = -1, .best_arr = {0}, .best_index = lo };
    if (hi <= lo) return best;

    uint64_t total_work = hi - lo;
    int threads_per_block = 256;
    int blocks = (total_work + threads_per_block - 1) / threads_per_block;
    if (blocks > 4096) blocks = 4096;

    if (blocks > allocated_blocks) {
        if (d_best_and_bid) {
            HIP_CHECK(hipFree(d_best_and_bid));
            HIP_CHECK(hipFree(d_indices));
            HIP_CHECK(hipFree(d_arrays));
        }
        HIP_CHECK(hipMalloc(&d_best_and_bid, sizeof(unsigned long long)));
        HIP_CHECK(hipMalloc(&d_indices, blocks * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&d_arrays, blocks * 25 * sizeof(uint8_t)));
        allocated_blocks = blocks;
    }

    HIP_CHECK(hipMemset(d_best_and_bid, 0, sizeof(unsigned long long)));

    compute_kernel<<<blocks, threads_per_block>>>(seed, lo, total_work, d_best_and_bid, d_indices, d_arrays);
    HIP_CHECK(hipDeviceSynchronize());

    unsigned long long host_best_and_bid;
    HIP_CHECK(hipMemcpy(&host_best_and_bid, d_best_and_bid, sizeof(unsigned long long), hipMemcpyDeviceToHost));

    uint32_t top_score = (uint32_t)(host_best_and_bid >> 32);
    uint32_t top_block = (uint32_t)(host_best_and_bid & 0xFFFFFFFF);

    uint64_t best_idx;
    uint8_t best_raw_arr[25];

    HIP_CHECK(hipMemcpy(&best_idx, &d_indices[top_block], sizeof(uint64_t), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(best_raw_arr, &d_arrays[top_block * 25], 25 * sizeof(uint8_t), hipMemcpyDeviceToHost));

    best.best_correct = (int)top_score;
    best.best_index = best_idx;
    for (int i = 0; i < 25; i++) {
        best.best_arr[i] = (int)best_raw_arr[i];
    }

    return best;
}
