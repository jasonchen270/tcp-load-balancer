#include "server.h"
#include "poller.h"
#include "connection.h"
#include "pool.h"
#include "health.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* How often to run an active health sweep. */
#define HEALTH_INTERVAL_MS 2000

/* Set by SIGINT/SIGTERM to request a graceful shutdown. */
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Monotonic milliseconds, for scheduling health sweeps independent of wall time. */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- small socket helpers ---- */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen"); close(fd); return -1;
    }
    if (set_nonblocking(fd) < 0) {
        perror("fcntl(listener)"); close(fd); return -1;
    }
    return fd;
}

/* Start a non-blocking connect to the backend. Returns fd, or -1.
 * The connect may still be in progress (EINPROGRESS); that's fine, and we treat
 * write-readiness on the fd as "connected". */
static int start_backend_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket(backend)"); return -1; }
    if (set_nonblocking(fd) < 0) { close(fd); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid backend host: %s\n", host);
        close(fd);
        return -1;
    }

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("connect(backend)");
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* ---- connection lifecycle ---- */

static pool_t *g_pool;
static long g_active_conns = 0;   /* live proxied connections (for drain + stats) */
static long g_total_conns = 0;    /* lifetime accepted connections (stats) */
static long g_max_conns = 0;      /* 0 = unlimited; otherwise cap on concurrent */
static long g_idle_timeout_ms = 0;/* 0 = disabled; reap connections idle this long */

/* Head of the intrusive list of all live connections. */
static conn_t *g_conns = NULL;

static void conns_insert(conn_t *c) {
    c->prev = NULL;
    c->next = g_conns;
    if (g_conns) g_conns->prev = c;
    g_conns = c;
}

static void conns_remove(conn_t *c) {
    if (c->prev) c->prev->next = c->next; else g_conns = c->next;
    if (c->next) c->next->prev = c->prev;
    c->prev = c->next = NULL;
}

/* Compute the poller interest for one end of a connection:
 *  - want READ if that side can still deliver data
 *  - want WRITE if there are bytes buffered FROM the peer waiting to go out
 * The buffer feeding writes to end i is buf[i^1].
 *
 * Special case: while the backend's non-blocking connect is still pending we
 * must watch it for WRITE (that's how we learn the connect completed), and we
 * must NOT yet read from the client into a buffer we can't drain, but reading
 * is harmless since it just fills the buffer; the real rule is that we don't
 * attempt to write to the backend until it's connected. */
static unsigned interest_for(conn_t *c, int i) {
    unsigned ev = 0;
    if (c->read_open[i]) ev |= POLL_READ;

    if (i == CONN_BACKEND && !c->backend_connected) {
        /* Pending connect: wait for writability to detect completion. Don't
         * read from the backend yet either. */
        return POLL_WRITE;
    }

    int peer = i ^ 1;
    if (c->end[peer] > c->start[peer]) ev |= POLL_WRITE;
    return ev;
}

static void update_interest(poller_t *p, conn_t *c) {
    if (c->fd[CONN_CLIENT]  >= 0)
        poller_mod(p, c->fd[CONN_CLIENT],  interest_for(c, CONN_CLIENT),  &c->handle[CONN_CLIENT]);
    if (c->fd[CONN_BACKEND] >= 0)
        poller_mod(p, c->fd[CONN_BACKEND], interest_for(c, CONN_BACKEND), &c->handle[CONN_BACKEND]);
}

static void conn_close(poller_t *p, conn_t *c) {
    if (c->closed) return;
    c->closed = true;
    for (int i = 0; i < 2; i++) {
        if (c->fd[i] >= 0) {
            poller_del(p, c->fd[i]);
            close(c->fd[i]);
            c->fd[i] = -1;
        }
    }
    /* Single teardown site: release the backend slot here so the pool's live
     * counts stay accurate for least-connections. */
    pool_release(g_pool, c->backend_idx);
    conns_remove(c);
    g_active_conns--;
    free(c);
}

/* Accept everything pending on the listener (level-ish: drain until EAGAIN). */
static void handle_accept(poller_t *p, int listen_fd) {
    for (;;) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cli, &len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* drained */
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        if (set_nonblocking(client_fd) < 0) { close(client_fd); continue; }
        int yes = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        /* Enforce a max concurrent connection cap if configured. */
        if (g_max_conns > 0 && g_active_conns >= g_max_conns) {
            close(client_fd); /* shed load; client sees a closed connection */
            continue;
        }

        /* Choose a healthy backend per the active policy. */
        int idx = pool_pick(g_pool);
        if (idx < 0) { close(client_fd); continue; } /* no healthy backend */
        backend_t *be = &g_pool->backends[idx];

        int backend_fd = start_backend_connect(be->host, be->port);
        if (backend_fd < 0) { close(client_fd); continue; }

        conn_t *c = calloc(1, sizeof(*c));
        if (!c) { close(client_fd); close(backend_fd); continue; }
        c->fd[CONN_CLIENT] = client_fd;
        c->fd[CONN_BACKEND] = backend_fd;
        c->handle[CONN_CLIENT]  = (conn_end_t){ .conn = c, .end = CONN_CLIENT };
        c->handle[CONN_BACKEND] = (conn_end_t){ .conn = c, .end = CONN_BACKEND };
        c->backend_idx = idx;
        c->read_open[CONN_CLIENT] = true;
        c->read_open[CONN_BACKEND] = true;
        c->backend_connected = false;
        c->last_activity = now_ms();

        /* Count this connection against the chosen backend; conn_close releases. */
        pool_acquire(g_pool, idx);
        conns_insert(c);
        g_active_conns++;
        g_total_conns++;

        /* Watch client for reads; watch backend for write-readiness so we learn
         * when the in-progress connect completes. Register per-end handles. */
        poller_add(p, client_fd, interest_for(c, CONN_CLIENT), &c->handle[CONN_CLIENT]);
        poller_add(p, backend_fd, interest_for(c, CONN_BACKEND), &c->handle[CONN_BACKEND]);
    }
}

/* Try to flush buffered bytes destined for end `to`. Source buffer is buf[from]
 * where from = to^1. Returns false if the connection should be closed.
 *
 * If `to` is the backend and its connect hasn't completed yet, we hold the data
 * in the buffer and flush it later (when the connect resolves), since writing now
 * would fail with ENOTCONN. */
static bool pump_write(conn_t *c, int to) {
    if (to == CONN_BACKEND && !c->backend_connected) return true;

    int from = to ^ 1;
    while (c->start[from] < c->end[from]) {
        size_t avail = c->end[from] - c->start[from];
        ssize_t w = write(c->fd[to], c->buf[from] + c->start[from], avail);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* socket full */
            if (errno == EINTR) continue;
            return false; /* real error */
        }
        c->start[from] += (size_t)w;
    }
    /* Fully drained: reset buffer offsets. If the source side has closed for
     * reading and its buffer is now empty, propagate EOF to `to`. */
    if (c->start[from] == c->end[from]) {
        c->start[from] = c->end[from] = 0;
        if (!c->read_open[from]) {
            shutdown(c->fd[to], SHUT_WR);
        }
    }
    return true;
}

/* Read from end `from` into its buffer, then attempt to push it to the peer.
 * Returns false if the connection should be closed. */
static bool pump_read(conn_t *c, int from) {
    int to = from ^ 1;
    /* Read while there's buffer space. */
    while (c->end[from] < CONN_BUFSZ) {
        ssize_t n = read(c->fd[from], c->buf[from] + c->end[from],
                         CONN_BUFSZ - c->end[from]);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            /* EOF from this side. Stop reading it; flush remaining then EOF. */
            c->read_open[from] = false;
            break;
        }
        c->end[from] += (size_t)n;
    }
    return pump_write(c, to);
}

/* Returns true if the connection is fully done and should be closed. */
static bool conn_finished(conn_t *c) {
    bool client_done  = !c->read_open[CONN_CLIENT]  && c->start[CONN_CLIENT]  == c->end[CONN_CLIENT];
    bool backend_done = !c->read_open[CONN_BACKEND] && c->start[CONN_BACKEND] == c->end[CONN_BACKEND];
    return client_done && backend_done;
}

/* The backend became writable for the first time: the non-blocking connect has
 * resolved. Check SO_ERROR to see whether it actually succeeded. Returns false
 * if the connect failed (caller should close). */
static bool finish_backend_connect(conn_t *c) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c->fd[CONN_BACKEND], SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        return false;
    if (err != 0) {
        errno = err;
        perror("backend connect");
        return false;
    }
    c->backend_connected = true;
    return true;
}

static void handle_conn_event(poller_t *p, conn_t *c, int end, unsigned events) {
    if (c->closed) return;

    /* Resolve a pending backend connect before any relaying. */
    if (end == CONN_BACKEND && !c->backend_connected) {
        if (events & POLL_WRITE) {
            if (!finish_backend_connect(c)) { conn_close(p, c); return; }
        } else {
            /* Shouldn't relay yet; just refresh interest. */
            update_interest(p, c);
            return;
        }
    }

    bool ok = true;
    if (events & POLL_WRITE) ok = pump_write(c, end);          /* drain to this fd */
    if (ok && (events & POLL_READ)) ok = pump_read(c, end);    /* read from this fd */

    /* The connect-completion above may have unblocked client->backend data that
     * was buffered while we waited; flush it now. */
    if (ok && end == CONN_BACKEND) ok = pump_write(c, CONN_BACKEND);

    if (!ok || conn_finished(c)) {
        conn_close(p, c);
        return;
    }
    c->last_activity = now_ms();
    update_interest(p, c);
}

/* Close any connection idle longer than g_idle_timeout_ms. */
static void reap_idle(poller_t *p) {
    if (g_idle_timeout_ms <= 0) return;
    long cutoff = now_ms() - g_idle_timeout_ms;
    conn_t *c = g_conns;
    while (c) {
        conn_t *next = c->next; /* conn_close frees c, so grab next first */
        if (c->last_activity < cutoff) conn_close(p, c);
        c = next;
    }
}

int run_server(const server_config_t *cfg, pool_t *pool) {
    g_pool = pool;
    g_max_conns = cfg->max_conns;
    g_idle_timeout_ms = cfg->idle_timeout_ms;

    int listen_fd = make_listener(cfg->listen_port);
    if (listen_fd < 0) return 1;

    poller_t *p = poller_create();
    if (!p) { fprintf(stderr, "poller_create failed\n"); close(listen_fd); return 1; }

    /* Graceful shutdown on Ctrl-C / SIGTERM; ignore SIGPIPE so a peer closing
     * mid-write surfaces as EPIPE from write() rather than killing us. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* The listener has no conn_t; we tag it with a sentinel pointer so the loop
     * can distinguish it from connection fds. */
    static const char LISTENER_TAG;
    poller_add(p, listen_fd, POLL_READ, (void *)&LISTENER_TAG);

    printf("lb listening on 127.0.0.1:%d, policy=%s, %d backend(s):\n",
           cfg->listen_port, policy_name(pool->policy), pool->count);
    for (int i = 0; i < pool->count; i++)
        printf("  - %s:%d\n", pool->backends[i].host, pool->backends[i].port);

    /* Run an initial health sweep so we start with accurate state. */
    health_sweep(pool);
    long next_health = now_ms() + HEALTH_INTERVAL_MS;
    long next_stats = now_ms() + 10000;
    bool draining = false;

    poll_event_t evs[64];
    for (;;) {
        /* Begin graceful shutdown once requested: stop accepting new clients. */
        if (g_stop && !draining) {
            draining = true;
            printf("\nshutting down: no longer accepting; draining %ld connection(s)\n",
                   g_active_conns);
            poller_del(p, listen_fd);
            close(listen_fd);
            listen_fd = -1;
        }
        if (draining && g_active_conns == 0) break; /* drained, exit cleanly */

        /* Wait only as long as the next scheduled task (health/stats), so timers
         * fire on time even when idle. */
        long now = now_ms();
        long until_health = next_health - now;
        long timeout = until_health < 0 ? 0 : until_health;
        if (g_idle_timeout_ms > 0 && g_idle_timeout_ms < timeout) timeout = g_idle_timeout_ms;

        int n = poller_wait(p, evs, 64, (int)timeout);
        if (n < 0) { perror("poller_wait"); break; }

        for (int i = 0; i < n; i++) {
            if (evs[i].udata == &LISTENER_TAG) {
                if (!draining) handle_accept(p, listen_fd);
            } else {
                conn_end_t *h = (conn_end_t *)evs[i].udata;
                handle_conn_event(p, h->conn, h->end, evs[i].events);
            }
        }

        now = now_ms();
        if (now >= next_health) { health_sweep(pool); next_health = now + HEALTH_INTERVAL_MS; }
        reap_idle(p);
        if (now >= next_stats) {
            printf("stats: active=%ld total=%ld\n", g_active_conns, g_total_conns);
            next_stats = now + 10000;
        }
    }

    printf("shutdown complete (served %ld connections)\n", g_total_conns);
    if (listen_fd >= 0) close(listen_fd);
    poller_destroy(p);
    return 0;
}
