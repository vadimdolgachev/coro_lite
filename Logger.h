#ifndef CORO_LITE_LOGGER_H
#define CORO_LITE_LOGGER_H

#include <chrono>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#ifndef CORO_LITE_VERBOSE_LOG
#ifdef NDEBUG
#define CORO_LITE_VERBOSE_LOG 0
#else
#define CORO_LITE_VERBOSE_LOG 1
#endif
#endif

#ifndef CORO_LITE_LOG_TAG
#define CORO_LITE_LOG_TAG "CoroLite"
#endif


inline const std::string &threadId() {
    thread_local const std::string id = [] {
        std::ostringstream os;
        os << std::this_thread::get_id();
        return os.str();
    }();
    return id;
}

#ifdef __ANDROID__
#include <android/log.h>

namespace CoroLite {
    template<typename... Args>
    void log_d(std::format_string<Args...> fmt, Args &&...args) {
        auto msg = std::format("[{}] {}", threadId(), std::format(fmt, std::forward<Args>(args)...));
        __android_log_print(ANDROID_LOG_DEBUG, CORO_LITE_LOG_TAG, "%s", msg.c_str());
    }

    template<typename... Args>
    void log_v(std::format_string<Args...> fmt, Args &&...args) {
        if constexpr (CORO_LITE_VERBOSE_LOG != 0) {
            auto msg = std::format("[{}] {}", threadId(), std::format(fmt, std::forward<Args>(args)...));
            __android_log_print(ANDROID_LOG_VERBOSE, CORO_LITE_LOG_TAG, "%s", msg.c_str());
        }
    }

    template<typename... Args>
    void log_e(std::format_string<Args...> fmt, Args &&...args) {
        auto msg = std::format("[{}] {}", threadId(), std::format(fmt, std::forward<Args>(args)...));
        __android_log_print(ANDROID_LOG_ERROR, CORO_LITE_LOG_TAG, "%s", msg.c_str());
    }
} // namespace CoroLite

#else

#include <syncstream>

namespace CoroLite {
    template<typename... Args>
    void log_d(std::format_string<Args...> fmt, Args &&...args) {
        std::osyncstream(std::cout) << std::format("{} [{}] {}",
                                                   floor<std::chrono::milliseconds>(std::chrono::system_clock::now()),
                                                   threadId(), std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void log_v(std::format_string<Args...> fmt, Args &&...args) {
        if constexpr (CORO_LITE_VERBOSE_LOG != 0) {
            log_d(fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void log_e(std::format_string<Args...> fmt, Args &&...args) {
        std::osyncstream(std::cerr) << std::format("{} [{}] {}",
                                                   floor<std::chrono::milliseconds>(std::chrono::system_clock::now()),
                                                   threadId(), std::format(fmt, std::forward<Args>(args)...));
    }
} // namespace CoroLite

#endif

#endif // CORO_LITE_LOGGER_H
