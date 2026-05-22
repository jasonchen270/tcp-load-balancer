#ifndef LB_SERVER_H
#define LB_SERVER_H

#include "pool.h"

/* Runtime knobs for the server. Zero means "use the default / disabled". */
typedef struct {
    int listen_port;
    long max_conns;        /* cap on concurrent connections; 0 = unlimited */
    long idle_timeout_ms;  /* reap connections idle this long; 0 = disabled */
} server_config_t;

/*
 * Event-driven (non-blocking) TCP load balancer.
 *
 * run_server() binds a listener, then runs a single-threaded event loop over
 * the poller, handling many concurrent client<->backend connections at once.
 * New connections are distributed across the pool's healthy backends per its
 * policy; dead backends are detected by periodic health checks and ejected.
 * On SIGINT/SIGTERM it stops accepting and drains in-flight connections.
 * Returns nonzero only on a fatal setup error.
 */
int run_server(const server_config_t *cfg, pool_t *pool);

#endif /* LB_SERVER_H */
