#ifndef CORO_LITE_TESTCHECK_H
#define CORO_LITE_TESTCHECK_H

#include <cstdio>
#include <cstdlib>
#include <source_location>

inline void check(const bool condition, const std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "%s:%u: check failed\n", location.file_name(), location.line());
        std::abort();
    }
}

#endif // CORO_LITE_TESTCHECK_H
