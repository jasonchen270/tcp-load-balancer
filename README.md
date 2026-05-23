# tcp-load-balancer

A single-threaded, event-driven **Layer-4 (TCP) load balancer** written in C,
built from raw sockets. It accepts client connections, selects a backend by a
configurable policy, splices bytes in both directions, and ejects dead backends
via health checks. This is the core model behind nginx and HAProxy, implemented from
scratch.

The focus is low-level systems programming: BSD sockets, non-blocking I/O, and
an event loop over `kqueue` (macOS/BSD), with an `epoll` (Linux) backend behind
the same interface.

## Build

```bash
cmake -S . -B build
cmake --build build
```

This produces binaries in `build/`:

- `lb`: the load balancer.
- `echo_backend`: a trivial TCP echo server used as a test backend.
- `concurrent_client`, `distribution_probe`: test tools.

The poller backend is chosen automatically: `epoll` on Linux, `kqueue` on
macOS/BSD.

## Tests

```bash
# concurrency: open N simultaneous connections, verify each round-trips
./tests/smoke_test.sh 100

# distribution: see which backend serves each of N simultaneous connections
./build/echo_backend 9001 & ./build/echo_backend 9002 & ./build/echo_backend 9003 &
./build/lb 8080 127.0.0.1:9001 127.0.0.1:9002 127.0.0.1:9003 &
./build/distribution_probe 127.0.0.1 8080 30
```

The kqueue (macOS) path is exercised by the above. The `epoll` (Linux) backend
shares all logic above the `poller.h` interface; to validate it, build and run
the same tests on Linux (e.g. a container):

```bash
docker run --rm -it -v "$PWD":/src -w /src gcc:14 \
  bash -lc 'apt-get update && apt-get install -y cmake && \
            cmake -S . -B build && cmake --build build && ./tests/smoke_test.sh 100'
```

## Usage

```
lb <listen_port> [--policy rr|leastconn] [--max-conns N] [--idle-timeout SECS] \
   <host:port> [<host:port> ...]
lb --config <file>
```

- Policies: `rr` (round-robin, default), `leastconn` (least-connections).
- `--max-conns N` caps concurrent connections; `--idle-timeout SECS` reaps idle ones.
- Backends are health-checked continuously; dead ones are ejected and rejoin on recovery.
- SIGINT/SIGTERM triggers a graceful shutdown (stop accepting, drain, exit).

Config-file form (`lb --config lb.conf`):

```
listen 8080
policy leastconn
max-conns 1024
idle-timeout 60
backend 127.0.0.1:9001
backend 127.0.0.1:9002
```

## Try it

```bash
# start three backends
./build/echo_backend 9001 & ./build/echo_backend 9002 & ./build/echo_backend 9003 &

# run lb in front of them
./build/lb 8080 127.0.0.1:9001 127.0.0.1:9002 127.0.0.1:9003

# in another terminal, each connection is sent to a backend by the policy:
nc 127.0.0.1 8080          # reply is tagged with which backend served you
```

## Requirements

- A C11 compiler (Apple clang or gcc)
- CMake ≥ 3.20
- macOS/BSD (`kqueue` backend) or Linux (`epoll` backend), selected
  automatically at build time

## License

MIT. See [LICENSE](LICENSE).
