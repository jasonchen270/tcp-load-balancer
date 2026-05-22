cmake_minimum_required(VERSION 3.20)
project(tcp_load_balancer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict warnings to surface as many potential issues at compile time as possible.
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# Release builds optimize; default to a sane mode if none specified.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")

# ---- The load balancer ----
# Pick the poller backend for this platform.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LB_POLLER src/poller_epoll.c)
else()
    set(LB_POLLER src/poller_kqueue.c)
endif()

add_executable(lb
    src/main.c
    src/server.c
    ${LB_POLLER}
    src/pool.c
    src/health.c
    src/config.c
)

# ---- Test backend ----
# A trivial echo server to point the load balancer at.
add_executable(echo_backend
    tests/echo_backend.c
)

# ---- Concurrency tester ----
# Opens N simultaneous connections and checks each round-trips correctly.
add_executable(concurrent_client
    tests/concurrent_client.c
)

# ---- Distribution probe ----
# Opens N connections at once and reports which backend served each, to observe
# the balancing policy in action.
add_executable(distribution_probe
    tests/distribution_probe.c
)

# Convenience: `cmake --build build --target run_lb`
add_custom_target(run_lb
    COMMAND lb
    DEPENDS lb
    COMMENT "Run the load balancer"
)
