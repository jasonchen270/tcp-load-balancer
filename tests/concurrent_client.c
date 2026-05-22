/*
 * concurrent_client.c opens N simultaneous connections to the load balancer,
 * sends a unique message on each, and verifies each connection gets that same
 * message echoed back (proving the backend was reached and the relay is
 * per-connection correct under concurrency).
 *
 * All N sockets are opened first and kept open together, so the load balancer
 * genuinely has N connections live at once, the thing a blocking server
 * cannot do.
 *
 * Usage: ./concurrent_client <host> <port> <N>
 * Exit:  0 if all N round-trips succeed, 1 otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* Read until we either find `needle` in the accumulated bytes or hit EOF. */
static int read_until(int fd, const char *needle) {
    char acc[8192];
    size_t len = 0;
    for (;;) {
        if (len >= sizeof(acc) - 1) break;
        ssize_t n = read(fd, acc + len, sizeof(acc) - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        acc[len] = '\0';
        if (strstr(acc, needle)) return 1;
    }
    acc[len] = '\0';
    return strstr(acc, needle) ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <host> <port> <N>\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int N = atoi(argv[3]);
    if (N <= 0 || N > 4096) { fprintf(stderr, "bad N\n"); return 2; }

    int *fds = calloc((size_t)N, sizeof(int));
    if (!fds) return 2;

    /* Phase 1: open all N connections at once. */
    int opened = 0;
    for (int i = 0; i < N; i++) {
        fds[i] = connect_to(host, port);
        if (fds[i] < 0) {
            fprintf(stderr, "connection %d failed\n", i);
            continue;
        }
        opened++;
    }
    fprintf(stderr, "opened %d/%d simultaneous connections\n", opened, N);

    /* Phase 2: send a unique message on each open connection, half-close write. */
    for (int i = 0; i < N; i++) {
        if (fds[i] < 0) continue;
        char msg[64];
        int m = snprintf(msg, sizeof(msg), "conn-%d-payload\n", i);
        write(fds[i], msg, (size_t)m);
        shutdown(fds[i], SHUT_WR); /* signal we're done sending */
    }

    /* Phase 3: verify each connection echoed its own message back. */
    int pass = 0;
    for (int i = 0; i < N; i++) {
        if (fds[i] < 0) continue;
        char needle[64];
        snprintf(needle, sizeof(needle), "conn-%d-payload", i);
        if (read_until(fds[i], needle)) pass++;
        close(fds[i]);
    }

    free(fds);
    printf("%d / %d connections echoed correctly\n", pass, N);
    return (pass == N) ? 0 : 1;
}
