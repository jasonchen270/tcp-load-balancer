# Engineering Decisions

> Records the *why* behind notable choices, with alternatives and tradeoffs.
> New decisions are appended as the project evolves.

## 1. Layer 4 (TCP) rather than Layer 7 (HTTP)

**Decision**: Balance raw TCP byte streams; do not parse application protocols.

**Alternatives considered**:
- L7/HTTP proxy: parse requests and route by path/host/header (like nginx).

**Reasoning**: L4 is protocol-agnostic and keeps the focus on the systems-level
core (sockets, non-blocking I/O, and the event loop) instead of HTTP parsing.
It works for *any* TCP protocol (HTTP, Redis, Postgres, SSH).

**Tradeoffs**: No content-based routing and no per-request load balancing on a
keep-alive connection (we balance per *connection*). Acceptable for the goals
of this project.

## 2. Single-threaded event loop rather than thread-per-connection

**Decision**: One thread, non-blocking sockets, an event loop over kqueue/epoll.

**Alternatives considered**:
- Thread (or process) per connection with blocking I/O, the simplest to write.
- Thread pool with a shared accept queue.

**Reasoning**: The event-loop model is how production proxies (nginx, HAProxy,
Envoy) actually scale, and learning it is the entire point. A single thread
sidesteps lock complexity and context-switch overhead, and scales to many
thousands of connections.

**Tradeoffs**: Harder to write correctly, since every operation must be non-blocking
and resumable, and one slow callback stalls everything. We also don't use
multiple cores (a real deployment would run one event loop per core).

## 3. kqueue first, with a poller abstraction for epoll later

**Decision**: Define a `poller` interface and implement a `kqueue` backend for
macOS now; leave a clean seam for an `epoll` backend on Linux.

**Reasoning**: Development happens on macOS (Darwin), where `epoll` does not
exist. Abstracting the readiness mechanism keeps the rest of the code portable
and makes the OS-specific part swappable, a useful, real-world pattern.

**Tradeoffs**: A small amount of indirection vs. calling kqueue directly. Worth
it for portability and clarity.

## 4. Per-end handle as the poller user pointer (not the raw fd)

**Decision**: Register a small `{conn, end}` handle as each fd's poller user
pointer, rather than relying on the poller to return the raw fd.

**Reasoning**: kqueue returns the fd in each event (`ident`), but epoll's
`epoll_event.data` is a *union*: you get either the fd or your pointer, not
both. Carrying the connection and which end (client/backend) inside the user
pointer makes the event-handling code identical on both backends.

**Tradeoffs**: Two extra small structs per connection. Negligible, and it
removes a portability landmine.

## 5. Health checks driven by the event-loop timeout (no OS timer)

**Decision**: Schedule periodic health sweeps by shortening the `poller_wait`
timeout, instead of registering an OS timer (`EVFILT_TIMER` / `timerfd`).

**Reasoning**: Both kqueue and epoll honor the wait timeout, so this needs no
platform-specific timer code and keeps the `poller` interface minimal. Health
checks are infrequent (seconds), so the precision is more than enough.

**Tradeoffs**: The sweep does brief blocking connects (bounded by a select
timeout). Acceptable for a handful of backends checked every few seconds; a
high-backend-count deployment would make the probes fully async.

## 6. Health hysteresis (rise/fall thresholds)

**Decision**: Require N consecutive failures to mark a backend DOWN and M
consecutive successes to bring it back UP, rather than flipping on each probe.

**Reasoning**: A single dropped probe (transient blip) shouldn't eject a healthy
backend, and one lucky success shouldn't readmit a flapping one. Hysteresis is
the standard way real load balancers avoid thrashing.

**Tradeoffs**: A genuinely-dead backend takes a few intervals to be ejected,
and a recovered one a few to rejoin. The delay is the price of stability.
