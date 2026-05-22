/*
 * poller_kqueue.c is the kqueue backend for the poller interface (macOS/BSD).
 *
 * kqueue tracks read and write interest as two separate filters (EVFILT_READ,
 * EVFILT_WRITE). To change interest we must issue EV_ADD for a filter we want
 * and EV_DELETE for one we no longer want, but EV_DELETE on a filter that was
 * never registered fails with ENOENT, and a failing change in a batch can cause
 * the kernel to skip the rest of the batch. To avoid that, we remember the
 * currently-registered mask per fd and only emit the precise transitions
 * needed (never deleting a filter that isn't registered).
 */

#include "poller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>

/* Per-fd record of which filters are currently registered. Indexed by fd. */
struct poller {
    int kq;
    unsigned *cur;   /* cur[fd] = currently registered mask (POLL_READ|WRITE) */
    int cap;         /* allocated size of cur[] */
};

poller_t *poller_create(void) {
    poller_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->kq = kqueue();
    if (p->kq < 0) { free(p); return NULL; }
    p->cap = 256;
    p->cur = calloc((size_t)p->cap, sizeof(unsigned));
    if (!p->cur) { close(p->kq); free(p); return NULL; }
    return p;
}

void poller_destroy(poller_t *p) {
    if (!p) return;
    if (p->kq >= 0) close(p->kq);
    free(p->cur);
    free(p);
}

static bool ensure_cap(poller_t *p, int fd) {
    if (fd < p->cap) return true;
    int newcap = p->cap;
    while (newcap <= fd) newcap *= 2;
    unsigned *n = realloc(p->cur, (size_t)newcap * sizeof(unsigned));
    if (!n) return false;
    memset(n + p->cap, 0, (size_t)(newcap - p->cap) * sizeof(unsigned));
    p->cur = n;
    p->cap = newcap;
    return true;
}

/* Move fd from its current registered mask to `want`, issuing only the needed
 * EV_ADD / EV_DELETE transitions. */
static bool apply(poller_t *p, int fd, unsigned want, void *udata) {
    if (fd < 0) return false;
    if (!ensure_cap(p, fd)) return false;

    unsigned have = p->cur[fd];
    struct kevent ch[2];
    int n = 0;

    /* READ filter transition */
    if ((want & POLL_READ) && !(have & POLL_READ))
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, udata);
    else if (!(want & POLL_READ) && (have & POLL_READ))
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, udata);

    /* WRITE filter transition */
    if ((want & POLL_WRITE) && !(have & POLL_WRITE))
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, udata);
    else if (!(want & POLL_WRITE) && (have & POLL_WRITE))
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, udata);

    if (n > 0) {
        if (kevent(p->kq, ch, n, NULL, 0, NULL) < 0) {
            if (errno != ENOENT) return false;
        }
    }
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
    struct kevent evs[64];
    if (max_events > 64) max_events = 64;

    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(p->kq, NULL, 0, evs, max_events, tsp);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        out[i].fd = (int)evs[i].ident;
        out[i].udata = evs[i].udata;
        out[i].events = 0;
        if (evs[i].filter == EVFILT_READ)  out[i].events |= POLL_READ;
        if (evs[i].filter == EVFILT_WRITE) out[i].events |= POLL_WRITE;
    }
    return n;
}
