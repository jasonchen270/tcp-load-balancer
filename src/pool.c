#include "pool.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void pool_init(pool_t *p, policy_t policy) {
    memset(p, 0, sizeof(*p));
    p->policy = policy;
}

bool pool_add(pool_t *p, const char *host_port) {
    if (p->count >= POOL_MAX_BACKENDS) return false;

    /* Split "host:port" on the last ':' so IPv4 literals are unambiguous. */
    const char *colon = strrchr(host_port, ':');
    if (!colon || colon == host_port) return false;

    size_t hostlen = (size_t)(colon - host_port);
    if (hostlen >= sizeof(p->backends[0].host)) return false;

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return false;

    backend_t *b = &p->backends[p->count];
    memcpy(b->host, host_port, hostlen);
    b->host[hostlen] = '\0';
    b->port = port;
    b->live = 0;
    b->healthy = true; /* assume up until a health check says otherwise */
    b->fails = 0;
    b->passes = 0;

    p->count++;
    return true;
}

int pool_pick(pool_t *p) {
    if (p->count == 0) return -1;

    switch (p->policy) {
    case POLICY_ROUND_ROBIN: {
        /* Advance the cursor, skipping unhealthy backends. Scan at most `count`
         * entries so we return -1 if none are healthy. */
        for (int tried = 0; tried < p->count; tried++) {
            int idx = p->next;
            p->next = (p->next + 1) % p->count;
            if (p->backends[idx].healthy) return idx;
        }
        return -1;
    }
    case POLICY_LEAST_CONN: {
        int best = -1;
        for (int i = 0; i < p->count; i++) {
            if (!p->backends[i].healthy) continue;
            if (best < 0 || p->backends[i].live < p->backends[best].live)
                best = i;
        }
        return best;
    }
    }
    return -1; /* unreachable */
}

bool pool_mark_health(pool_t *p, int idx, bool ok) {
    if (idx < 0 || idx >= p->count) return false;
    backend_t *b = &p->backends[idx];

    if (ok) {
        b->fails = 0;
        if (!b->healthy) {
            if (++b->passes >= HEALTH_RISE_THRESHOLD) {
                b->healthy = true;
                b->passes = 0;
                return true; /* came back up */
            }
        }
    } else {
        b->passes = 0;
        if (b->healthy) {
            if (++b->fails >= HEALTH_FALL_THRESHOLD) {
                b->healthy = false;
                b->fails = 0;
                return true; /* went down */
            }
        }
    }
    return false;
}

void pool_acquire(pool_t *p, int idx) {
    if (idx >= 0 && idx < p->count) p->backends[idx].live++;
}

void pool_release(pool_t *p, int idx) {
    if (idx >= 0 && idx < p->count && p->backends[idx].live > 0)
        p->backends[idx].live--;
}

const char *policy_name(policy_t policy) {
    switch (policy) {
    case POLICY_ROUND_ROBIN: return "round-robin";
    case POLICY_LEAST_CONN:  return "least-connections";
    }
    return "?";
}
