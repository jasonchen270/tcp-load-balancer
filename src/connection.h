#ifndef LB_CONNECTION_H
#define LB_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A proxied connection pairs a client socket with a backend socket and relays
 * bytes between them, non-blocking. Each direction has its own buffer holding
 * data that has been read from one side but not yet fully written to the other
 * (non-blocking writes can be partial).
 *
 * Naming: an "end" is one side of the pair. end[CLIENT] and end[BACKEND].
 */

#define CONN_CLIENT  0
#define CONN_BACKEND 1

#define CONN_BUFSZ 65536

typedef struct conn conn_t;

/*
 * Per-end poller handle. We register one of these (not the bare conn) as the
 * poller's user pointer for each fd, so an event identifies both the connection
 * AND which end fired, without relying on the poller to return the raw fd
 * (kqueue does, epoll doesn't). end is CONN_CLIENT or CONN_BACKEND.
 */
typedef struct {
    conn_t *conn;
    int end;
} conn_end_t;

struct conn {
    int fd[2];              /* fd[CONN_CLIENT], fd[CONN_BACKEND] */
    conn_end_t handle[2];   /* per-end poller handles (udata) */
    int backend_idx;        /* which pool backend this connection uses */
    long last_activity;     /* monotonic ms of last read/write, for idle timeout */
    bool read_open[2];      /* can this side still deliver data? */
    bool backend_connected; /* backend's non-blocking connect has completed */
    bool closed;            /* whole connection torn down */

    /* Intrusive list links so the server can walk all live connections
     * (for idle-timeout sweeps and graceful drain). */
    struct conn *prev;
    struct conn *next;

    /*
     * buf[i] holds bytes read FROM end i that are pending write TO end (i^1).
     * [start, end) is the unwritten region.
     */
    char buf[2][CONN_BUFSZ];
    size_t start[2];
    size_t end[2];
};

#endif /* LB_CONNECTION_H */
