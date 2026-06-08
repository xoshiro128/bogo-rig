#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "worker.h"
#include "compute.h"
#include "server.h"
#include "dashboard.h"

static job              current_job = {0};
static _Atomic uint64_t lease_gen = 0;
static _Atomic uint64_t total_done_all = 0;
static _Atomic bool     started = false;
static _Atomic int      global_best_correct_atomic = -1;

static pthread_mutex_t  lease_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   lease_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t       *workers = nullptr;
static pthread_t        reporter;

static compute_result   global_best = { .best_correct = -1 };
static pthread_mutex_t  best_mutex  = PTHREAD_MUTEX_INITIALIZER;

static void *worker_thread(void *arg) {
    int tid = (int)(intptr_t)arg;
    uint64_t my_gen = 0;

    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        job j;
        uint64_t cur_gen;

        pthread_mutex_lock(&lease_mutex);
        while ((cur_gen = atomic_load_explicit(&lease_gen, memory_order_acquire)) == my_gen &&
               atomic_load_explicit(&running, memory_order_relaxed)) {
            pthread_cond_wait(&lease_cond, &lease_mutex);
        }
        my_gen = cur_gen;
        j = current_job;
        pthread_mutex_unlock(&lease_mutex);

        if (!atomic_load_explicit(&running, memory_order_relaxed)) break;

        uint64_t i = (uint64_t)tid * chunk_size;

        while (atomic_load_explicit(&running, memory_order_relaxed) && i < j.count) {
            if (atomic_load_explicit(&lease_gen, memory_order_acquire) != my_gen) {
                break;
            }

            uint64_t lo = i;
            uint64_t hi = lo + chunk_size < j.count ? lo + chunk_size : j.count;

            compute_result cr = compute_run(j.seed, lo, hi);

            atomic_fetch_add_explicit(&total_done_all, hi - lo, memory_order_relaxed);

            if (cr.best_correct > atomic_load_explicit(&global_best_correct_atomic, memory_order_relaxed)) {
                pthread_mutex_lock(&best_mutex);
                if (cr.best_correct > global_best.best_correct) {
                    global_best = cr;
                    atomic_store_explicit(&global_best_correct_atomic, cr.best_correct, memory_order_relaxed);
                }
                pthread_mutex_unlock(&best_mutex);
            }

            i += (uint64_t)num_workers * chunk_size;
        }
    }
    return nullptr;
}

static void *reporter_thread(void *arg) {
    (void)arg;
    struct timespec sleep_time = {1, 0};
    uint64_t last_reported_total = 0;
    uint64_t last_reported_gen = 0;

    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        nanosleep(&sleep_time, nullptr);

        pthread_mutex_lock(&lease_mutex);
        uint64_t current_gen = atomic_load_explicit(&lease_gen, memory_order_relaxed);
        job active_job = current_job;
        pthread_mutex_unlock(&lease_mutex);

        if (current_gen != last_reported_gen) {
            last_reported_total = 0;
            last_reported_gen = current_gen;
        }

        uint64_t current_total = atomic_load_explicit(&total_done_all, memory_order_relaxed);

        if (current_total <= last_reported_total) continue;

        result r = {0};
        bool has_best = false;

        pthread_mutex_lock(&best_mutex);
        if (global_best.best_correct >= 0) {
            r.seed = active_job.seed;
            r.total_done = current_total;
            r.best_correct = global_best.best_correct;
            r.best_index = global_best.best_index;
            memcpy(r.best_arr, global_best.best_arr, sizeof(r.best_arr));
            has_best = true;
        }
        pthread_mutex_unlock(&best_mutex);

        if (has_best) {
            server_send_result(&r);
            last_reported_total = current_total;
        }
    }
    return nullptr;
}

void worker_push_job(job j) {
    pthread_mutex_lock(&lease_mutex);
    current_job = j;
    atomic_fetch_add_explicit(&lease_gen, 1, memory_order_release);
    pthread_mutex_unlock(&lease_mutex);

    atomic_store_explicit(&total_done_all, 0, memory_order_relaxed);

    pthread_mutex_lock(&best_mutex);
    global_best.best_correct = -1;
    atomic_store_explicit(&global_best_correct_atomic, -1, memory_order_relaxed);
    pthread_mutex_unlock(&best_mutex);

    bool expected = false;
    if (atomic_compare_exchange_strong(&started, &expected, true)) {
        dashboard_log("Starting %d worker(s) + 1 reporter thread", num_workers);
        workers = malloc(num_workers * sizeof(pthread_t));
        for (int i = 0; i < num_workers; i++) {
            pthread_create(&workers[i], nullptr, worker_thread, (void *)(intptr_t)i);
        }
        pthread_create(&reporter, nullptr, reporter_thread, nullptr);
    }

    pthread_cond_broadcast(&lease_cond);
}
