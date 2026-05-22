#ifndef LB_POOL_H
#define LB_POOL_H

#include <stdbool.h>

/*
 * A pool of backend servers and the policy for choosing among them.
 *
 * The event loop calls pool_pick() to choose a backend for a new connection,
 * pool_acquire() to mark that backend as having one more live connection, and
 * pool_release() when the connection tears down. Keeping acquire/release paired
 * is what makes the least-connections policy accurate; release is called from
 * the single connection-teardown site.
 */

#define POOL_MAX_BACKENDS 64

/* Health hysteresis: how many consecutive failures mark a backend down, and how
 * many consecutive successes bring it back up. Avoids flapping on one blip. */
#define HEALTH_FALL_THRESHOLD 3
#define HEALTH_RISE_THRESHOLD 2

typedef enum {
    POLICY_ROUND_ROBIN,
    POLICY_LEAST_CONN,
} policy_t;

typedef struct {
    char host[64];
    int port;
    int live;        /* current live connections assigned to this backend */
    bool healthy;    /* currently considered up (eligible for new connections) */
    int fails;       /* consecutive failed health checks */
    int passes;      /* consecutive passed health checks (while unhealthy) */
} backend_t;

typedef struct {
    backend_t backends[POOL_MAX_BACKENDS];
    int count;
    policy_t policy;
    int next;        /* rotating cursor for round-robin */
} pool_t;

/* Initialize an empty pool with the given policy. */
void pool_init(pool_t *p, policy_t policy);

/* Add a backend "host:port". Returns false if full or malformed. */
bool pool_add(pool_t *p, const char *host_port);

/* Choose a healthy backend index for a new connection per the policy, or -1 if
 * none are available. Does not change live counts; call pool_acquire() once the
 * connection is actually being set up. */
int pool_pick(pool_t *p);

/* Adjust the live-connection count for a backend index. */
void pool_acquire(pool_t *p, int idx);
void pool_release(pool_t *p, int idx);

/* Record a health probe result for a backend; applies the rise/fall thresholds
 * and flips healthy state. Returns true if the healthy state changed. */
bool pool_mark_health(pool_t *p, int idx, bool ok);

const char *policy_name(policy_t policy);

#endif /* LB_POOL_H */
