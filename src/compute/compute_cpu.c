#include "compute.h"
#include <stdint.h>

static inline uint32_t rotl32(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

compute_result compute_run(uint64_t seed, uint64_t lo, uint64_t hi) {
    compute_result best = { .best_correct = -1, .best_index = lo };

    for (uint64_t idx = lo; idx < hi; idx++) {
        uint64_t state = seed + idx * 0x9e3779b97f4a7c15ULL;

        state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        uint64_t a = z ^ (z >> 31);

        state += 0x9e3779b97f4a7c15ULL;
        z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        uint64_t b = z ^ (z >> 31);

        uint32_t s[4] = {
            (uint32_t)(a & 0xffffffff),
            (uint32_t)(a >> 32),
            (uint32_t)(b & 0xffffffff),
            (uint32_t)(b >> 32)
        };

        if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;

        int arr[25] = {
             1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
            11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
            21, 22, 23, 24, 25
        };

        for (uint32_t i = 24; i >= 1; i--) {
            uint32_t max = i + 1;
            uint32_t thr = (uint32_t)(-max) % max;
            uint32_t x;

            do {
                uint32_t res = rotl32(s[0] + s[3], 7) + s[0];
                uint32_t t = s[1] << 9;
                s[2] ^= s[0];
                s[3] ^= s[1];
                s[1] ^= s[2];
                s[0] ^= s[3];
                s[2] ^= t;
                s[3] = rotl32(s[3], 11);
                x = res;
            } while (x < thr);

            uint32_t j = x % max;
            int tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }

        int correct = 0;
        for (int i = 0; i < 25; i++) {
            if (arr[i] == i + 1) correct++;
        }

        if (correct > best.best_correct) {
            best.best_correct = correct;
            best.best_index = idx;
            for (int i = 0; i < 25; i++) best.best_arr[i] = arr[i];

            if (correct == 25) break;
        }
    }

    return best;
}
