#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include "dashboard.h"
#include "server.h"

#define LOG_LINES 8
#define LOG_LINE_MAX 256

static char log_buf[LOG_LINES][LOG_LINE_MAX];
static int log_head = 0;
static int log_count = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void dashboard_log(const char *fmt, ...) {
    pthread_mutex_lock(&log_mutex);

    char *line = log_buf[log_head];
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    int offset = strftime(line, LOG_LINE_MAX, "[%H:%M:%S] ", &t);

    va_list args;
    va_start(args, fmt);
    vsnprintf(line + offset, LOG_LINE_MAX - offset, fmt, args);
    va_end(args);

    log_head = (log_head + 1) % LOG_LINES;
    if (log_count < LOG_LINES) log_count++;

    pthread_mutex_unlock(&log_mutex);
}

void* dashboard_run(void* arg) {
    (void)arg;
    while (running) {
        char snap[LOG_LINES][LOG_LINE_MAX];
        int count, head;

        pthread_mutex_lock(&log_mutex);
        count = log_count;
        head = log_head;
        memcpy(snap, log_buf, sizeof(log_buf));
        pthread_mutex_unlock(&log_mutex);

        pthread_mutex_lock(&stats_mutex);
        stats s = global_stats;
        pthread_mutex_unlock(&stats_mutex);

        printf("\033[H\033[J");
        int start = (count == LOG_LINES) ? head : 0;
        for (int i = 0; i < count; i++) {
            printf("\033[36m%s\033[0m\n", snap[(start + i) % LOG_LINES]);
        }
        printf("--------------------------------\n");
        printf("rate:  %llu shuffles/sec\n", (unsigned long long)s.rate);
        printf("total: %llu lifetime\n", (unsigned long long)s.lifetime_shuffles);
        printf("best:  %d / 25\n", s.all_time_best);

        fflush(stdout);
        sleep(1);
    }
    return nullptr;
}
