#ifndef LB_CONFIG_H
#define LB_CONFIG_H

#include <stdbool.h>
#include "server.h"
#include "pool.h"

/*
 * Load configuration from a simple line-based file into cfg and pool.
 *
 * Format (one directive per line; '#' starts a comment):
 *   listen 8080
 *   policy leastconn          # rr | leastconn
 *   max-conns 1024            # optional
 *   idle-timeout 60           # optional, seconds
 *   backend 127.0.0.1:9001
 *   backend 127.0.0.1:9002
 *
 * Returns true on success. On failure, prints an error and returns false.
 */
bool config_load(const char *path, server_config_t *cfg, pool_t *pool);

#endif /* LB_CONFIG_H */
