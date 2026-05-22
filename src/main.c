/*
 * main.c is the entry point for the TCP load balancer.
 *
 * Two ways to configure it:
 *
 *   1) command line:
 *      lb <listen_port> [--policy rr|leastconn] [--max-conns N]
 *         [--idle-timeout SECS] <host:port> [<host:port> ...]
 *
 *   2) config file:
 *      lb --config <file>
 *
 * The event loop then distributes connections across the pool's healthy
 * backends per the chosen policy.
 */

#include "server.h"
#include "pool.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "usage:\n"
        "  %s <listen_port> [--policy rr|leastconn] [--max-conns N]\n"
        "     [--idle-timeout SECS] <host:port> [<host:port> ...]\n"
        "  %s --config <file>\n"
        "\n"
        "policies: rr (round-robin, default), leastconn (least-connections)\n"
        "examples:\n"
        "  %s 8080 127.0.0.1:9001 127.0.0.1:9002\n"
        "  %s 8080 --policy leastconn --max-conns 1024 127.0.0.1:9001 127.0.0.1:9002\n"
        "  %s --config lb.conf\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) { usage(argv[0]); return 1; }

    server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    pool_t pool;

    /* Config-file mode. */
    if (strcmp(argv[1], "--config") == 0) {
        if (argc != 3) { usage(argv[0]); return 1; }
        if (!config_load(argv[2], &cfg, &pool)) return 1;
        return run_server(&cfg, &pool);
    }

    /* Command-line mode. */
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid listen port: %s (must be 1..65535)\n", argv[1]);
        return 1;
    }
    cfg.listen_port = port;

    policy_t policy = POLICY_ROUND_ROBIN;
    int argi = 2;

    /* Parse optional flags, in any order, before the backend list. */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--policy") == 0 && argi + 1 < argc) {
            const char *pol = argv[++argi];
            if (strcmp(pol, "rr") == 0)             policy = POLICY_ROUND_ROBIN;
            else if (strcmp(pol, "leastconn") == 0) policy = POLICY_LEAST_CONN;
            else { fprintf(stderr, "unknown policy: %s\n", pol); return 1; }
        } else if (strcmp(argv[argi], "--max-conns") == 0 && argi + 1 < argc) {
            cfg.max_conns = atol(argv[++argi]);
        } else if (strcmp(argv[argi], "--idle-timeout") == 0 && argi + 1 < argc) {
            cfg.idle_timeout_ms = atol(argv[++argi]) * 1000;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[argi]);
            usage(argv[0]);
            return 1;
        }
        argi++;
    }

    if (argi >= argc) {
        fprintf(stderr, "error: at least one backend (host:port) is required\n");
        usage(argv[0]);
        return 1;
    }

    pool_init(&pool, policy);
    for (; argi < argc; argi++) {
        if (!pool_add(&pool, argv[argi])) {
            fprintf(stderr, "invalid backend: %s (expected host:port)\n", argv[argi]);
            return 1;
        }
    }

    return run_server(&cfg, &pool);
}
