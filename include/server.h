#pragma once
#include <libwebsockets.h>
#include <stdint.h>
#include "worker.h"

#define WS_HOST "bogo.swapjs.dev"
#define WS_PORT 443
#define WS_PATH "/ws"

typedef struct {
    const char *uuid;
    const char *nickname;
    const char *code;
} credentials;

typedef struct {
    struct lws *wsi;
    bool       open;
} connection;

typedef struct {
    uint64_t lifetime_shuffles;
    int all_time_best;
    uint64_t rate; // per sec
} stats;
extern stats global_stats;
extern pthread_mutex_t stats_mutex;

extern _Atomic bool     running;
extern pthread_mutex_t  shutdown_mutex;
extern pthread_cond_t   shutdown_cond;

void server_init(const credentials *creds);
void server_connect(void);
void server_send_result(const result *r);
void *server_lws_thread(void *arg);
void client_shutdown(void);
