# coro_lite

A small C++23 coroutine runtime for Linux and Android. It combines an epoll-based scheduler, a worker pool, cancellable
timers, non-blocking TCP sockets, and IPv4/IPv6/hostname endpoint handling in the `CoroLite` namespace.

## Requirements

- Linux or Android (the runtime uses `epoll`, `eventfd`, and `timerfd`)
- GCC 11 or newer, or Clang 14 or newer
- CMake 3.22.1 or newer
- POSIX threads

## Add to a CMake Project

Add the repository as a subdirectory and link its namespaced target:

```cmake
add_subdirectory(third_party/coro_lite)
target_link_libraries(my_app PRIVATE CoroLite::coro_lite)
```

The target publishes its include directory and requires C++23. Set `SANITIZER=address` or `SANITIZER=thread` when
configuring a parent project to instrument the library.

## Basic Usage

```cpp
#include "CoroTask.h"

using namespace CoroLite;

CoroTask<> run(CancellationTokenSource &shutdown) {
    co_await TimerAwaiter{std::chrono::seconds(1), {}};
    shutdown.requestStop();
}

int main() {
    CancellationTokenSource cancellation;
    EpollScheduler scheduler(cancellation);
    run(cancellation).startDetached(scheduler);
    scheduler.run();
}
```

Call `CancellationTokenSource::requestStop()` to wake the scheduler and begin shutdown.
`run()` is single-use. Destroying a scheduler on another thread requests cancellation and waits for
`run()` and its workers to finish. Destruction from one of its own workers is a contract violation
and terminates the process. User code resumed on workers must cooperate with cancellation and return.
The caller remains responsible for joining its own thread after destruction returns.

A `CoroTask` is single-use: move it into `co_await`, or call exactly one of `start()` and `startDetached()`.

## Components

- `CoroTask<T>` propagates results and exceptions between coroutines.
- `EpollScheduler` waits for file-descriptor readiness and resumes work on its thread pool.
- `CancellationTokenSource` and `CancellationToken` provide shared, file-descriptor-backed cancellation.
- `Socket` exposes awaiters for connect, accept, read, and write operations.
- `TimerAwaiter` provides cancellable delays; `Endpoint` represents IP addresses and hostnames.
- `UniqueFd` provides move-only file-descriptor ownership.

## Socket and Timer Lifetimes

`Socket::read()` and `write()` return `CoroTask<size_t>`; existing `co_await socket->read(...)`
and `co_await socket->write(...)` calls retain their syntax. They keep the socket alive and retry
`EAGAIN` by awaiting readiness again. Cancellation and other errors propagate to the caller.
The low-level socket awaiters still represent one I/O attempt.

Sockets keep a weak reference to fd registrations and may outlive the scheduler. Cancel and finish
outstanding operations before explicitly closing a socket; concurrent close and I/O on the same socket
are not supported. Direct registrations made with `add()` remain the caller's responsibility.
`addAwaiter()` atomically registers an operation and its optional cancellation fd, with rollback on error.

`TimerAwaiter` leases timerfds from the scheduler, caching up to 64 idle timers. Zero delays complete
immediately without a timerfd; negative delays throw `invalid_argument`. Cancelled leases are disarmed
and drained before reuse. `Timer::arm()`/`armSec()` can throw `system_error` and are no longer `noexcept`;
`Timer::arm(0ns)` explicitly disarms a standalone timer. Cancellation readiness remains set after stop,
so one waiter cannot consume another waiter's notification.

## Logging

`CORO_LITE_VERBOSE_LOG` accepts `AUTO` (default: Debug on, other configurations off), `ON`, or `OFF`.
`CORO_LITE_LOG_TAG` defaults to `CoroLite` on Android. These CMake definitions propagate to consumers
so inline logging functions use the same configuration. Applications can set the tag before
`add_subdirectory()`. Errors go to stderr on Linux; Android uses the corresponding Logcat priorities.
Thread IDs are cached per thread.

## Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCORO_LITE_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For sanitizers, add `-DSANITIZER=address` or `-DSANITIZER=thread`. Use Debug to enable runtime assertions; the new regression checks also remain active with `NDEBUG`.
The seven CTest cases cover coroutine ownership, cancellation isolation, socket lifetimes and I/O retries,
concurrent scheduler destruction, FIFO execution, deferred readiness, timer reuse and cleanup, syscall
failures, and logging with verbose enabled and disabled. Regression tests use Linux linker wrappers;
production code has no syscall-injection hooks.

Coroutine protocol hooks intentionally remain instance members. IDE suggestions to make stateless
hooks static conflict with clang-tidy diagnostics for the compiler-generated instance calls; these
style suggestions do not indicate a runtime error.
