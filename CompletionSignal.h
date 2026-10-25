#ifndef CORO_LITE_COMPLETIONSIGNAL_H
#define CORO_LITE_COMPLETIONSIGNAL_H

#include <cerrno>
#include <system_error>

#include <sys/eventfd.h>
#include <unistd.h>

#include "FdUtils.h"

namespace CoroLite {
    class CompletionSignal final {
    public:
        CompletionSignal() {
            fd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
            if (fd.get() < 0) {
                throw std::system_error(errno, std::system_category(), "eventfd failed");
            }
        }

        void signal() const noexcept {
            constexpr eventfd_t val = 1;
            while (write(fd.get(), &val, sizeof(val)) < 0 && errno == EINTR) {
            }
        }

        void drain() const noexcept {
            eventfd_t val;
            while (read(fd.get(), &val, sizeof(val)) < 0 && errno == EINTR) {
            }
        }

        [[nodiscard]] int getFd() const noexcept {
            return fd.get();
        }

    private:
        UniqueFd fd;
    };
} // namespace CoroLite

#endif // CORO_LITE_COMPLETIONSIGNAL_H
