/*
 * echo_backend.c is a trivial TCP echo server (forks a child per connection).
 *
 * Purpose: a dead-simple, trustworthy "backend" to point the load balancer at.
 * It listens on a port and, for each connection, forks a child that greets with
 * the backend's port and echoes back whatever bytes it receives. Forking keeps
 * it concurrent so a held-open connection can't starve the accept loop; the
 * load-balancer tests hold many connections open at once. The port tag lets us
 * SEE which backend served a request when the LB distributes across several.
 *
 * This is intentionally NOT the interesting code; it's the test rig. The
 * load balancer itself is where the event-loop/non-blocking work happens.
 *
 * Usage:  ./echo_backend <port>
 * Test:   nc 127.0.0.1 <port>   (type a line, see it echoed back)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "error: port must be 1..65535\n");
        return 1;
    }

    /* AF_INET = IPv4, SOCK_STREAM = TCP. Returns a file descriptor. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    /* SO_REUSEADDR lets us rebind the port immediately after restart,
     * instead of waiting out the kernel's TIME_WAIT state. Essential for
     * iterative development where you restart constantly. */
    int yes = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only */
    addr.sin_port = htons((uint16_t)port);          /* host->network byte order */

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    /* Backlog of 16 pending connections is plenty for a test backend. */
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        return 1;
    }

    printf("echo_backend listening on 127.0.0.1:%d\n", port);

    /* Reap finished children so they don't pile up as zombies. */
    signal(SIGCHLD, SIG_IGN);

    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue; /* keep serving despite a transient accept error */
        }

        /* Fork a child per connection so one idle/long-lived connection can't
         * starve the accept loop. This makes the backend genuinely concurrent,
         * which the load-balancer tests rely on (they hold many connections
         * open at once). */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(conn_fd); continue; }
        if (pid == 0) {
            /* Child: serve this one connection, then exit. */
            close(listen_fd);

            char greeting[64];
            int n = snprintf(greeting, sizeof(greeting),
                             "[backend:%d] connected\n", port);
            write(conn_fd, greeting, (size_t)n);

            char buf[4096];
            ssize_t r;
            while ((r = read(conn_fd, buf, sizeof(buf))) > 0) {
                ssize_t written = 0;
                while (written < r) {
                    ssize_t w = write(conn_fd, buf + written, (size_t)(r - written));
                    if (w < 0) { perror("write"); break; }
                    written += w;
                }
            }
            close(conn_fd);
            _exit(0);
        }

        /* Parent: close its copy of the connection and accept the next. */
        close(conn_fd);
    }

    /* unreachable, but tidy */
    close(listen_fd);
    return 0;
}
