#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Trim leading/trailing whitespace in place; returns pointer into s. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

bool config_load(const char *path, server_config_t *cfg, pool_t *pool) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "config: cannot open %s\n", path); return false; }

    memset(cfg, 0, sizeof(*cfg));
    policy_t policy = POLICY_ROUND_ROBIN;
    pool_init(pool, policy);
    bool have_listen = false;
    bool have_backend = false;

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;

        /* Strip comments and surrounding whitespace. */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *s = trim(line);
        if (*s == '\0') continue;

        /* Split into "key value". */
        char *sp = s;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (*sp == '\0') {
            fprintf(stderr, "config: line %d: missing value\n", lineno);
            fclose(f); return false;
        }
        *sp = '\0';
        char *key = s;
        char *val = trim(sp + 1);

        if (strcmp(key, "listen") == 0) {
            cfg->listen_port = atoi(val);
            if (cfg->listen_port <= 0 || cfg->listen_port > 65535) {
                fprintf(stderr, "config: line %d: bad listen port\n", lineno);
                fclose(f); return false;
            }
            have_listen = true;
        } else if (strcmp(key, "policy") == 0) {
            if (strcmp(val, "rr") == 0)             pool->policy = POLICY_ROUND_ROBIN;
            else if (strcmp(val, "leastconn") == 0) pool->policy = POLICY_LEAST_CONN;
            else { fprintf(stderr, "config: line %d: unknown policy '%s'\n", lineno, val);
                   fclose(f); return false; }
        } else if (strcmp(key, "max-conns") == 0) {
            cfg->max_conns = atol(val);
        } else if (strcmp(key, "idle-timeout") == 0) {
            cfg->idle_timeout_ms = atol(val) * 1000;
        } else if (strcmp(key, "backend") == 0) {
            if (!pool_add(pool, val)) {
                fprintf(stderr, "config: line %d: bad backend '%s'\n", lineno, val);
                fclose(f); return false;
            }
            have_backend = true;
        } else {
            fprintf(stderr, "config: line %d: unknown directive '%s'\n", lineno, key);
            fclose(f); return false;
        }
    }
    fclose(f);

    if (!have_listen) { fprintf(stderr, "config: missing 'listen'\n"); return false; }
    if (!have_backend) { fprintf(stderr, "config: at least one 'backend' required\n"); return false; }
    return true;
}
