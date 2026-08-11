#ifndef CORO_LITE_COMPLETIONSIGNAL_H
#define CORO_LITE_COMPLETIONSIGNAL_H

#include <cstdint>

#include <sys/eventfd.h>
#include <unistd.h>

#include "FdUtils.h"

// NOLINTBEGIN(readability-make-member-function-const)

namespace CoroLite {
    class CompletionSignal final {
    public:
        CompletionSignal() {
            fd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        }

        void signal() noexcept {
            constexpr uint64_t val = 1;
            [[maybe_unused]] ssize_t r = write(fd.get(), &val, sizeof(val));
        }

        void drain() noexcept {
            uint64_t val;
            [[maybe_unused]] ssize_t r = read(fd.get(), &val, sizeof(val));
        }

        [[nodiscard]] int getFd() const noexcept {
            return fd.get();
        }

    private:
        UniqueFd fd;
    };
} // namespace CoroLite

// NOLINTEND(readability-make-member-function-const)

#endif // CORO_LITE_COMPLETIONSIGNAL_H
