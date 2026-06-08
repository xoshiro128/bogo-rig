#pragma once
#include <stdint.h>

typedef struct {
    uint64_t seed;
    uint64_t count;
} job;

typedef struct {
    uint64_t seed;
    uint64_t total_done;
    uint64_t best_index;
    int      best_correct;
    int      best_arr[25];
    int      conn_id;
} result;

extern uint64_t chunk_size;
extern int      num_workers;

void worker_push_job(job j);
