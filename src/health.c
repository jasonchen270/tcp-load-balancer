#include "health.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define PROBE_TIMEOUT_MS 500

/* Attempt a TCP connect to host:port with a bounded timeout. Returns true if
 * the connect succeeds. Uses a non-blocking connect + select so a dead backend
 * doesn't stall the sweep for the full OS connect timeout. */
static bool probe(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return false; }

    int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (rc == 0) { close(fd); return true; } /* connected immediately */

    if (errno != EINPROGRESS) { close(fd); return false; }

    /* Wait for the socket to become writable (connect resolved). */
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {
        .tv_sec = PROBE_TIMEOUT_MS / 1000,
        .tv_usec = (PROBE_TIMEOUT_MS % 1000) * 1000,
    };

    rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) { close(fd); return false; } /* timeout or error */

    /* Writable: check whether the connect actually succeeded. */
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) { close(fd); return false; }

    close(fd);
    return err == 0;
}

void health_sweep(pool_t *pool) {
    for (int i = 0; i < pool->count; i++) {
        backend_t *b = &pool->backends[i];
        bool ok = probe(b->host, b->port);
        bool changed = pool_mark_health(pool, i, ok);
        if (changed) {
            printf("health: backend %s:%d is now %s\n",
                   b->host, b->port, b->healthy ? "UP" : "DOWN");
        }
    }
}
