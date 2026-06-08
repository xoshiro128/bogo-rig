#pragma once
#include <stdint.h>

typedef struct {
    int      best_correct;
    int      best_arr[25];
    uint64_t best_index;
} compute_result;

compute_result compute_run(uint64_t seed, uint64_t lo, uint64_t hi);
