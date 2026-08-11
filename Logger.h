#ifndef CORO_LITE_LOGGER_H
#define CORO_LITE_LOGGER_H

#include <format>
#include <iostream>
#include <sstream>
#include <thread>

constexpr bool PRINT_VERBOSE_LOG = true;


inline std::string threadId() {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

#ifdef __ANDROID__
#include <android/log.h>

namespace CoroLite {
    template<typename... Args>
    void log_d(std::format_string<Args...> fmt, Args &&... args) {
        auto msg = std::format("[{}] {}", threadId(), std::format(fmt, std::forward<Args>(args)...));
        __android_log_print(ANDROID_LOG_DEBUG, "SSHProxy", "%s", msg.c_str());
    }

    template<typename... Args>
    void log_v(std::format_string<Args...> fmt, Args &&... args) {
        if constexpr (PRINT_VERBOSE_LOG) {
            auto msg = std::format("[{}] {}", threadId(), std::format(fmt, std::forward<Args>(args)...));
            __android_log_print(ANDROID_LOG_VERBOSE, "SSHProxy", "%s", msg.c_str());
        }
    }

    template<typename... Args>
    void log_e(std::format_string<Args...> fmt, Args &&... args) {
        auto msg = std::format("[{}] {}", threadId(), std::format(fmt, std::forward<Args>(args)...));
        __android_log_print(ANDROID_LOG_ERROR, "SSHProxy", "%s", msg.c_str());
    }
} // namespace CoroLite

#else

#include <syncstream>

namespace CoroLite {
    template<typename... Args>
    void log_d(std::format_string<Args...> fmt, Args &&... args) {
        std::osyncstream(std::cout) << std::format("{} [{}] {}",
                                                   floor<std::chrono::milliseconds>(std::chrono::system_clock::now()),
                                                   threadId(),
                                                   std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void log_v(std::format_string<Args...> fmt, Args &&... args) {
        if constexpr (PRINT_VERBOSE_LOG) {
            log_d(fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void log_e(std::format_string<Args...> fmt, Args &&... args) {
        std::osyncstream(std::cout) << std::format("{} [{}] {}",
                                                   floor<std::chrono::milliseconds>(std::chrono::system_clock::now()),
                                                   threadId(),
                                                   std::format(fmt, std::forward<Args>(args)...));
    }
} // namespace CoroLite

#endif

#endif //CORO_LITE_LOGGER_H
