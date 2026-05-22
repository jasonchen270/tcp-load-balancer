/*
 * distribution_probe.c opens N connections through the load balancer (all
 * held open at once), reads the backend's greeting on each, and reports how
 * many connections each backend served.
 *
 * The echo backend greets with "[backend:PORT] connected", so parsing PORT tells
 * us which backend the load balancer chose. Holding all N open simultaneously is
 * what makes the result meaningful for least-connections: live counts genuinely
 * overlap, so we can see the policy spread load.
 *
 * Usage: ./distribution_probe <host> <port> <N>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
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
    /* Bound the greeting read so a slow/missing reply can't hang the probe. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Read the greeting and extract the backend port from "[backend:PORT]". */
static int read_backend_port(int fd) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *p = strstr(buf, "backend:");
    if (!p) return -1;
    return atoi(p + strlen("backend:"));
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s <host> <port> <N>\n", argv[0]); return 2; }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int N = atoi(argv[3]);
    if (N <= 0 || N > 4096) { fprintf(stderr, "bad N\n"); return 2; }

    int *fds = calloc((size_t)N, sizeof(int));
    int *ports = calloc((size_t)N, sizeof(int));
    if (!fds || !ports) return 2;

    /* Open all N at once so all are live simultaneously while the policy assigns
     * them, then let them settle before reading greetings. */
    for (int i = 0; i < N; i++)
        fds[i] = connect_to(host, port);
    usleep(300 * 1000); /* 300ms for backends to connect and greet */
    for (int i = 0; i < N; i++)
        ports[i] = (fds[i] >= 0) ? read_backend_port(fds[i]) : -1;

    /* Tally distinct backend ports. */
    int uniq_ports[64];
    int uniq_count[64];
    int u = 0;
    for (int i = 0; i < N; i++) {
        if (ports[i] < 0) continue;
        int found = -1;
        for (int j = 0; j < u; j++) if (uniq_ports[j] == ports[i]) { found = j; break; }
        if (found < 0 && u < 64) { found = u; uniq_ports[u] = ports[i]; uniq_count[u] = 0; u++; }
        if (found >= 0) uniq_count[found]++;
    }

    printf("distribution of %d connections across %d backend(s):\n", N, u);
    for (int j = 0; j < u; j++)
        printf("  backend:%d -> %d\n", uniq_ports[j], uniq_count[j]);

    for (int i = 0; i < N; i++) if (fds[i] >= 0) close(fds[i]);
    free(fds);
    free(ports);
    return 0;
}
