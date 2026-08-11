#ifndef CORO_LITE_CONSTS_H
#define CORO_LITE_CONSTS_H

#include <cstddef>

namespace CoroLite::Consts {
    constexpr size_t EPOLL_BATCH_SIZE = 16;
    constexpr size_t EPOLL_TIMEOUT_MS = 5'000;
    constexpr long long NANOSECONDS_PER_SECOND = 1'000'000'000LL;
    constexpr size_t THREAD_POOL_SIZE = 4;
}

#endif // CORO_LITE_CONSTS_H
