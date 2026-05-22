# Architecture

## Overview

A single-threaded, event-driven Layer-4 (TCP) load balancer. It sits between
clients and a pool of backend servers, accepts TCP connections, selects a
backend by a configurable policy, and shuttles bytes in both directions for the
life of the connection.

```
                    ┌─────────────────────────────┐
   client ───TCP───▶│   tcp-load-balancer (lb)    │───TCP───▶ backend A
   client ───TCP───▶│                             │───TCP───▶ backend B
   client ───TCP───▶│  one thread, one event loop │───TCP───▶ backend C
                    └─────────────────────────────┘
```

## Core model: one thread, one event loop

The whole program runs on a single thread. Instead of blocking on any one
socket, every socket is set non-blocking and registered with the OS event
notification facility (`kqueue` on macOS/BSD, `epoll` on Linux). The event loop
asks the kernel "which of my thousands of sockets are ready to read/write?" and
only touches those. This is how nginx/HAProxy serve huge connection counts
without a thread per client (the "C10k" approach).

## Components

| Component   | File                   | Responsibility                                         |
|-------------|------------------------|--------------------------------------------------------|
| Entry point | `src/main.c`           | Parse args / config, build the pool, start the server  |
| Server loop | `src/server.c`         | Listener, non-blocking accept/connect, the event loop, |
|             |                        | health scheduling, idle reaping, graceful shutdown     |
| Poller      | `src/poller.h`         | Abstraction over kqueue/epoll: add/mod/del/wait        |
| kqueue      | `src/poller_kqueue.c`  | macOS/BSD backend for the poller interface             |
| epoll       | `src/poller_epoll.c`   | Linux backend for the poller interface                 |
| Connection  | `src/connection.h`     | Per-connection pair state: fds, buffers, flags, links  |
| Backend pool| `src/pool.{h,c}`       | Backends + selection policy + live counts + health     |
| Health      | `src/health.{h,c}`     | Active TCP-connect probes; mark backends up/down       |
| Config      | `src/config.{h,c}`     | Parse a line-based config file                          |

The poller backend is chosen at build time by `CMakeLists.txt`: `epoll` on
Linux, `kqueue` elsewhere. The rest of the code is identical across platforms
because it only uses the `poller.h` interface, and identifies events via a
per-end handle registered as the user pointer, never relying on the poller to
return the raw fd (kqueue does; epoll doesn't).

## Balancing policies

Each new client connection is assigned a backend by `pool_pick()`:

- **round-robin** (default): a rotating cursor, `next = (next + 1) % count`.
  Even distribution across equal backends; keeps no per-backend load state.
- **least-connections**: pick the backend with the fewest live connections.
  Reacts to uneven connection lifetimes; relies on accurate live counts.

Live counts are maintained by `pool_acquire()` on accept and `pool_release()`
in `conn_close()`, the single teardown site, so the counts can't drift.
`pool_pick()` skips backends currently marked unhealthy.

## Health checks

The event loop runs a health sweep every few seconds (scheduled via the
`poller_wait` timeout, so no platform-specific timer is needed). Each backend
gets a short non-blocking TCP connect probe. Results feed `pool_mark_health()`,
which applies hysteresis (N consecutive failures mark a backend DOWN, M
consecutive successes bring it back UP) so a single blip doesn't flap a
backend in and out of rotation.

## Operational concerns

- **Graceful shutdown**: SIGINT/SIGTERM stops accepting, removes the listener,
  and drains in-flight connections before exiting.
- **Limits**: `--max-conns` caps concurrent connections (excess are shed).
- **Idle timeout**: connections idle past `--idle-timeout` are reaped. Live
  connections are tracked in an intrusive list so the sweep can walk them.
- **Stats**: periodic `active`/`total` connection counts to stdout.

## Connection lifecycle

1. Listener socket is readable → `accept()` new **client** connection(s),
   draining until `EAGAIN`.
2. Start a non-blocking `connect()` to the backend. The connect may be in
   progress (`EINPROGRESS`); we watch the backend fd for writability to learn
   when it completes, and check `SO_ERROR` to confirm it succeeded. Client data
   that arrives before the backend is connected is buffered, then flushed once
   the connect resolves.
3. Pair the two fds. Each readable side's bytes are buffered and written to the
   other side. Handle partial reads/writes and backpressure.
4. Either side closes/errors → tear down both fds, free buffers.
