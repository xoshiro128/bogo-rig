#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libwebsockets.h>
#include "server.h"
#include "worker.h"
#include "dashboard.h"
#include "cJSON.h"

#define SEND_QUEUE_SIZE 64

_Atomic bool    running        = true;
pthread_mutex_t stats_mutex    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  shutdown_cond  = PTHREAD_COND_INITIALIZER;
stats           global_stats   = {0};

static credentials     stored_creds;
static connection      conn;

static char            *send_queue[SEND_QUEUE_SIZE];
static int              send_head  = 0;
static int              send_tail  = 0;
static int              send_count = 0;
static pthread_mutex_t  send_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct lws_context *context = nullptr;

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
    void *user, void *in, size_t len);

static const struct lws_protocols protocols[] = {
    {"", ws_callback, 0, 0, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

void *server_lws_thread(void *arg) {
    (void)arg;
    while (running) {
        lws_service(context, 50);
    }
    lws_context_destroy(context);
    context = nullptr;
    return nullptr;
}

static bool queue_push(char *json) {
    pthread_mutex_lock(&send_mutex);
    if (send_count >= SEND_QUEUE_SIZE) {
        pthread_mutex_unlock(&send_mutex);
        dashboard_log("Send queue full, dropping message");
        free(json);
        return false;
    }
    send_queue[send_head] = json;
    send_head = (send_head + 1) % SEND_QUEUE_SIZE;
    send_count++;
    pthread_mutex_unlock(&send_mutex);
    lws_cancel_service(context);
    return true;
}

static char *queue_pop(void) {
    pthread_mutex_lock(&send_mutex);
    if (send_count == 0) {
        pthread_mutex_unlock(&send_mutex);
        return nullptr;
    }
    char *json = send_queue[send_tail];
    send_tail = (send_tail + 1) % SEND_QUEUE_SIZE;
    send_count--;
    pthread_mutex_unlock(&send_mutex);
    return json;
}

void server_init(const credentials *creds) {
    stored_creds = *creds;

    struct lws_context_creation_info info = {0};
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (context == nullptr) {
        dashboard_log("Failed to create libwebsocket context");
        exit(1);
    }
}

void server_connect(void) {
    struct lws_client_connect_info info = {0};
    info.context        = context;
    info.address        = WS_HOST;
    info.port           = WS_PORT;
    info.path           = WS_PATH;
    info.host           = WS_HOST;
    info.origin         = info.address;
    info.ssl_connection = LCCSCF_USE_SSL;
    info.protocol       = protocols[0].name;

    conn.wsi = lws_client_connect_via_info(&info);
    if (conn.wsi == nullptr) {
        dashboard_log("Failed to initiate connection");
        exit(1);
    }
}

void server_send_result(const result *r) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "result");

    char seed_str[32];
    snprintf(seed_str, sizeof(seed_str), "%llu", (unsigned long long)r->seed);
    cJSON_AddStringToObject(msg, "seed", seed_str);

    cJSON_AddNumberToObject(msg, "total_done",   (double)r->total_done);
    cJSON_AddNumberToObject(msg, "best_correct",  r->best_correct);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 25; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(r->best_arr[i]));
    cJSON_AddItemToObject(msg, "best_arr", arr);

    cJSON_AddNumberToObject(msg, "best_index", (double)r->best_index);

    queue_push(cJSON_PrintUnformatted(msg));
    cJSON_Delete(msg);
}

void client_shutdown(void) {
    pthread_mutex_lock(&shutdown_mutex);
    running = false;
    pthread_cond_broadcast(&shutdown_cond);
    pthread_mutex_unlock(&shutdown_mutex);
    if (context) lws_cancel_service(context);
}

static inline void server_send_hello(struct lws *wsi) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type",     "hello");
    cJSON_AddNumberToObject(msg, "v",         5);
    cJSON_AddStringToObject(msg, "uuid",     stored_creds.uuid);
    cJSON_AddStringToObject(msg, "nickname", stored_creds.nickname);
    cJSON_AddStringToObject(msg, "code",     stored_creds.code);

    queue_push(cJSON_PrintUnformatted(msg));
    cJSON_Delete(msg);
    lws_callback_on_writable(wsi);
}

static inline void parse_welcome(cJSON *msg) {
    const cJSON *uuid     = cJSON_GetObjectItemCaseSensitive(msg, "uuid");
    const cJSON *lifetime = cJSON_GetObjectItemCaseSensitive(msg, "lifetime_shuffles");
    const cJSON *best     = cJSON_GetObjectItemCaseSensitive(msg, "all_time_best");

    if (cJSON_IsString(uuid) && strcmp(uuid->valuestring, stored_creds.uuid) != 0)
        dashboard_log("New account uuid: %s", uuid->valuestring);

    pthread_mutex_lock(&stats_mutex);
    global_stats.lifetime_shuffles = (uint64_t)lifetime->valuedouble;
    global_stats.all_time_best     = best->valueint;
    uint64_t snap_lifetime = global_stats.lifetime_shuffles;
    int      snap_best     = global_stats.all_time_best;
    pthread_mutex_unlock(&stats_mutex);

    dashboard_log("Welcome. Lifetime shuffles: %llu, All time best: %d",
                  (unsigned long long)snap_lifetime, snap_best);
}

static inline void parse_job(cJSON *msg) {
    const cJSON *seed  = cJSON_GetObjectItemCaseSensitive(msg, "seed");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(msg, "count");
    if (!cJSON_IsString(seed) || !cJSON_IsNumber(count)) return;
    job j = {
        .seed  = strtoull(seed->valuestring, nullptr, 10),
        .count = (uint64_t)count->valuedouble,
    };
    dashboard_log("New job: seed=%s count=%llu", seed->valuestring, (unsigned long long)j.count);
    worker_push_job(j);
}

static inline void parse_credited(cJSON *msg) {
    const cJSON *lifetime = cJSON_GetObjectItemCaseSensitive(msg, "lifetime_shuffles");
    const cJSON *best     = cJSON_GetObjectItemCaseSensitive(msg, "all_time_best");
    const cJSON *rate     = cJSON_GetObjectItemCaseSensitive(msg, "rate");
    const cJSON *credit   = cJSON_GetObjectItemCaseSensitive(msg, "credit");

    pthread_mutex_lock(&stats_mutex);
    global_stats.lifetime_shuffles = (uint64_t)lifetime->valuedouble;
    global_stats.all_time_best     = best->valueint;
    global_stats.rate              = (uint64_t)rate->valuedouble;
    uint64_t snap_credit           = (uint64_t)credit->valuedouble;
    pthread_mutex_unlock(&stats_mutex);

    dashboard_log("Credited: +%llu", (unsigned long long)snap_credit);
}

static inline void parse_rejected(cJSON *msg) {
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(msg, "reason");
    dashboard_log("Rejected: %s", reason->valuestring);
}

static inline void parse_banned(cJSON *msg) {
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(msg, "reason");
    dashboard_log("Banned: %s", reason->valuestring);
    client_shutdown();
}

static inline void parse_contributions_closed(void) {
    dashboard_log("Contributions closed, shutting down");
    client_shutdown();
}

static inline void parse_client_outdated(void) {
    dashboard_log("Client outdated, please update");
    client_shutdown();
}

static void parse_message(void *in, size_t len) {
    cJSON *msg = cJSON_ParseWithLength(in, len);
    if (msg == nullptr) {
        dashboard_log("Failed to parse JSON");
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
    if (!cJSON_IsString(type)) {
        dashboard_log("Message has no type field");
        cJSON_Delete(msg);
        return;
    }
    const char *t = type->valuestring;
    if      (strcmp(t, "welcome")              == 0) parse_welcome(msg);
    else if (strcmp(t, "job")                  == 0) parse_job(msg);
    else if (strcmp(t, "credited")             == 0) parse_credited(msg);
    else if (strcmp(t, "rejected")             == 0) parse_rejected(msg);
    else if (strcmp(t, "banned")               == 0) parse_banned(msg);
    else if (strcmp(t, "contributions_closed") == 0) parse_contributions_closed();
    else if (strcmp(t, "client_outdated")      == 0) parse_client_outdated();
    else if (strcmp(t, "ping")                 == 0);
    else if (strcmp(t, "stats_tick")           == 0);
    else dashboard_log("Unknown message type: %s", t);
    cJSON_Delete(msg);
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
    void *user, void *in, size_t len) {
    (void)user;
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            conn.wsi = wsi;
            dashboard_log("Connected to server");
            server_send_hello(wsi);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            parse_message(in, len);
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            conn.wsi = nullptr;
            dashboard_log("Connection closed");
            client_shutdown();
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            conn.wsi = nullptr;
            dashboard_log("Connection error: %s", in ? (const char *)in : "unknown");
            client_shutdown();
            break;

        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if (conn.wsi != nullptr && running) {
                pthread_mutex_lock(&send_mutex);
                int has_data = send_count > 0;
                pthread_mutex_unlock(&send_mutex);
                if (has_data)
                    lws_callback_on_writable(conn.wsi);
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            char *json = queue_pop();
            if (json == nullptr) break;

            size_t jlen = strlen(json);
            if (jlen > 4096) {
                dashboard_log("Message too large (%zu bytes), dropping", jlen);
                free(json);
                break;
            }
            unsigned char buf[LWS_PRE + 4096];
            memcpy(&buf[LWS_PRE], json, jlen);
            free(json);

            lws_write(wsi, &buf[LWS_PRE], jlen, LWS_WRITE_TEXT);

            pthread_mutex_lock(&send_mutex);
            int remaining = send_count;
            pthread_mutex_unlock(&send_mutex);
            if (remaining > 0)
                lws_callback_on_writable(wsi);
            break;
        }

        default:
            break;
    }
    return 0;
}
