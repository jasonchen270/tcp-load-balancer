#ifndef LB_POLLER_H
#define LB_POLLER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A thin abstraction over the OS readiness-notification facility (kqueue on
 * macOS/BSD; an epoll backend can implement the same interface on Linux).
 *
 * Usage: create a poller, add fds with the events you care about, then call
 * poller_wait() to block until some are ready. Each event carries back the fd
 * and an opaque user pointer registered with it (used here to find the
 * connection a given fd belongs to).
 */

#define POLL_READ  0x1u
#define POLL_WRITE 0x2u

typedef struct poller poller_t;

typedef struct {
    int fd;
    unsigned events;   /* POLL_READ and/or POLL_WRITE that fired */
    void *udata;       /* user pointer registered for this fd */
} poll_event_t;

/* Create/destroy. Returns NULL on failure. */
poller_t *poller_create(void);
void poller_destroy(poller_t *p);

/* Register fd with the given interest mask and user pointer. */
bool poller_add(poller_t *p, int fd, unsigned events, void *udata);

/* Change the interest mask for an already-added fd. */
bool poller_mod(poller_t *p, int fd, unsigned events, void *udata);

/* Stop watching fd. */
bool poller_del(poller_t *p, int fd);

/*
 * Block until at least one fd is ready (or timeout_ms elapses; -1 = forever).
 * Fills out[] with up to max_events ready events. Returns the count, 0 on
 * timeout, or -1 on error.
 */
int poller_wait(poller_t *p, poll_event_t *out, int max_events, int timeout_ms);

#endif /* LB_POLLER_H */
