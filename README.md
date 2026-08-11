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
A `CoroTask` is single-use: move it into `co_await`, or call exactly one of `start()` and `startDetached()`.

## Components

- `CoroTask<T>` propagates results and exceptions between coroutines.
- `EpollScheduler` waits for file-descriptor readiness and resumes work on its thread pool.
- `CancellationTokenSource` and `CancellationToken` provide shared, file-descriptor-backed cancellation.
- `Socket` exposes awaiters for connect, accept, read, and write operations.
- `TimerAwaiter` provides cancellable delays; `Endpoint` represents IP addresses and hostnames.
- `UniqueFd` provides move-only file-descriptor ownership.

## Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCORO_LITE_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For an AddressSanitizer build, add `-DSANITIZER=address` during configuration. Tests currently cover coroutine ownership,
single-consumption rules, detached cleanup, nested suspension, and cancellation-driven scheduler shutdown.
