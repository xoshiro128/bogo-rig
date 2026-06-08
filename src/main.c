#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "server.h"
#include "worker.h"
#include "dashboard.h"

uint64_t chunk_size  = 1000000;
int      num_workers = 1;

int main(int argc, char *argv[]) {
    credentials account_creds = {0};

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--uuid") == 0) {
            if (++i >= argc) { fprintf(stderr, "--uuid requires a value\n"); return 1; }
            account_creds.uuid = argv[i];
        } else if (strcmp(argv[i], "--nickname") == 0) {
            if (++i >= argc) { fprintf(stderr, "--nickname requires a value\n"); return 1; }
            account_creds.nickname = argv[i];
        } else if (strcmp(argv[i], "--code") == 0) {
            if (++i >= argc) { fprintf(stderr, "--code requires a value\n"); return 1; }
            account_creds.code = argv[i];
        } else if (strcmp(argv[i], "--chunk-size") == 0) {
            if (++i >= argc) { fprintf(stderr, "--chunk-size requires a value\n"); return 1; }
            chunk_size = strtoull(argv[i], nullptr, 10);
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (++i >= argc) { fprintf(stderr, "--workers requires a value\n"); return 1; }
            char *end;
            num_workers = strtol(argv[i], &end, 10);
            if (*end != '\0' || num_workers <= 0) {
                fprintf(stderr, "--workers requires a positive integer\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s --uuid <uuid> --nickname <name> --code <code>\n"
                   "  --uuid       your account uuid\n"
                   "  --nickname   your display name\n"
                   "  --code       your account code (empty string if new account)\n"
                   "  --chunk-size shuffles per worker chunk (default 1000000)\n"
                   "  --workers    amount of workers (default 1)\n"
                   "you can get uuid by running \"localStorage\" in the browser console\n",
                   argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\nUse --help for usage.\n", argv[i]);
            return 1;
        }
    }

    if (!account_creds.uuid || !account_creds.nickname || !account_creds.code) {
        fprintf(stderr, "Error: --uuid, --nickname, and --code are required.\nUse --help for usage.\n");
        return 1;
    }

    pthread_t dash_thread, ws_thread;
    pthread_create(&dash_thread, nullptr, dashboard_run, nullptr);

    server_init(&account_creds);
    server_connect();

    pthread_create(&ws_thread, nullptr, server_lws_thread, nullptr);

    while(running) sleep(1);
    client_shutdown();

    pthread_join(ws_thread, nullptr);
    pthread_join(dash_thread, nullptr);

    return 0;
}
