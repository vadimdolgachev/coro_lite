#ifndef CORO_LITE_TIMER_H
#define CORO_LITE_TIMER_H

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <sys/timerfd.h>

#include "Consts.h"
#include "FdUtils.h"

namespace CoroLite {
    class Timer final {
    public:
        Timer() {
            fd.reset(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
            if (fd.get() < 0) {
                throw std::system_error(errno, std::system_category(), "timerfd_create failed");
            }
        }

        void arm(const std::chrono::nanoseconds delay) const {
            if (delay.count() < 0) {
                throw std::invalid_argument("Timer delay must not be negative");
            }
            const auto seconds = delay.count() / Consts::NANOSECONDS_PER_SECOND;
            if (seconds > std::numeric_limits<time_t>::max()) {
                throw std::invalid_argument("Timer delay exceeds time_t range");
            }
            itimerspec spec{};
            spec.it_value.tv_sec = static_cast<time_t>(seconds);
            spec.it_value.tv_nsec = static_cast<long>(delay.count() % Consts::NANOSECONDS_PER_SECOND);
            while (timerfd_settime(fd.get(), 0, &spec, nullptr) < 0) {
                const int error = errno;
                if (error == EINTR) {
                    continue;
                }
                throw std::system_error(error, std::system_category(), "timerfd_settime failed");
            }
        }

        void armSec(const int secs) const {
            arm(std::chrono::seconds(secs));
        }

        void drain() const noexcept {
            static_cast<void>(consume());
        }

        [[nodiscard]] bool consume() const noexcept {
            uint64_t val = 0;
            ssize_t result;
            do {
                result = read(fd.get(), &val, sizeof(val));
            } while (result < 0 && errno == EINTR);
            return result == sizeof(val) && val != 0;
        }

        [[nodiscard]] bool reset() const noexcept {
            const itimerspec spec{};
            int result;
            do {
                result = timerfd_settime(fd.get(), 0, &spec, nullptr);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                return false;
            }
            uint64_t value;
            ssize_t count;
            do {
                count = read(fd.get(), &value, sizeof(value));
            } while (count < 0 && errno == EINTR);
            return count == sizeof(value) || (count < 0 && errno == EAGAIN);
        }

        [[nodiscard]] int getFd() const noexcept {
            return fd.get();
        }

    private:
        UniqueFd fd;
    };
} // namespace CoroLite


#endif // CORO_LITE_TIMER_H
