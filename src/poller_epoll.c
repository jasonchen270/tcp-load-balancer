/*
 * poller_epoll.c is the epoll backend for the poller interface (Linux).
 *
 * Implements the same poller.h API as the kqueue backend. epoll differs from
 * kqueue in two ways that matter here:
 *   - Read and write interest are a single bitmask per fd (EPOLLIN/EPOLLOUT),
 *     changed atomically with EPOLL_CTL_MOD, with no separate filters to add/delete.
 *   - You must use EPOLL_CTL_ADD the first time an fd is registered and
 *     EPOLL_CTL_MOD afterwards, so we track which fds are currently registered.
 *
 * NOTE: this file is only compiled on Linux (selected in CMakeLists.txt). It is
 * written against the epoll API but has not been run on this macOS dev machine.
 */

#include "poller.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>

struct poller {
    int epfd;
    unsigned *cur;   /* cur[fd] = current interest mask; 0 = not registered */
    bool *reg;       /* reg[fd] = is fd currently in the epoll set? */
    int cap;
};

poller_t *poller_create(void) {
    poller_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->epfd = epoll_create1(0);
    if (p->epfd < 0) { free(p); return NULL; }
    p->cap = 256;
    p->cur = calloc((size_t)p->cap, sizeof(unsigned));
    p->reg = calloc((size_t)p->cap, sizeof(bool));
    if (!p->cur || !p->reg) { close(p->epfd); free(p->cur); free(p->reg); free(p); return NULL; }
    return p;
}

void poller_destroy(poller_t *p) {
    if (!p) return;
    if (p->epfd >= 0) close(p->epfd);
    free(p->cur);
    free(p->reg);
    free(p);
}

static bool ensure_cap(poller_t *p, int fd) {
    if (fd < p->cap) return true;
    int newcap = p->cap;
    while (newcap <= fd) newcap *= 2;

    /* Reassign into the struct as each realloc succeeds, so a partial failure
     * never leaves p->cur/p->reg pointing at freed memory. */
    unsigned *nc = realloc(p->cur, (size_t)newcap * sizeof(unsigned));
    if (!nc) return false;
    p->cur = nc;

    bool *nr = realloc(p->reg, (size_t)newcap * sizeof(bool));
    if (!nr) return false; /* p->cur already grown; harmless to leave larger */
    p->reg = nr;

    memset(p->cur + p->cap, 0, (size_t)(newcap - p->cap) * sizeof(unsigned));
    memset(p->reg + p->cap, 0, (size_t)(newcap - p->cap) * sizeof(bool));
    p->cap = newcap;
    return true;
}

static uint32_t to_epoll(unsigned events) {
    uint32_t e = 0;
    if (events & POLL_READ)  e |= EPOLLIN;
    if (events & POLL_WRITE) e |= EPOLLOUT;
    return e;
}

static bool apply(poller_t *p, int fd, unsigned want, void *udata) {
    if (fd < 0) return false;
    if (!ensure_cap(p, fd)) return false;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = to_epoll(want);
    ev.data.ptr = udata;

    int op;
    if (want == 0) {
        if (!p->reg[fd]) return true; /* nothing to remove */
        op = EPOLL_CTL_DEL;
    } else if (!p->reg[fd]) {
        op = EPOLL_CTL_ADD;
    } else {
        op = EPOLL_CTL_MOD;
    }

    if (epoll_ctl(p->epfd, op, fd, op == EPOLL_CTL_DEL ? NULL : &ev) < 0) {
        if (!(op == EPOLL_CTL_DEL && errno == ENOENT)) return false;
    }

    p->reg[fd] = (want != 0);
    p->cur[fd] = want;
    return true;
}

bool poller_add(poller_t *p, int fd, unsigned events, void *udata) {
    return apply(p, fd, events, udata);
}

bool poller_mod(poller_t *p, int fd, unsigned events, void *udata) {
    return apply(p, fd, events, udata);
}

bool poller_del(poller_t *p, int fd) {
    return apply(p, fd, 0, NULL);
}

int poller_wait(poller_t *p, poll_event_t *out, int max_events, int timeout_ms) {
    struct epoll_event evs[64];
    if (max_events > 64) max_events = 64;

    int n = epoll_wait(p->epfd, evs, max_events, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        out[i].udata = evs[i].data.ptr;
        out[i].events = 0;
        if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))  out[i].events |= POLL_READ;
        if (evs[i].events & EPOLLOUT)                          out[i].events |= POLL_WRITE;
        /* epoll doesn't return the fd directly when using data.ptr; the server
         * identifies the fd via the conn it points to. We set fd to -1 here. */
        out[i].fd = -1;
    }
    return n;
}
