#include "FdRegistry.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include <sys/epoll.h>

namespace CoroLite {
    FdRegistry::FdRegistry() :
        epollFd(epoll_create1(EPOLL_CLOEXEC)) {
        if (epollFd.get() < 0) {
            throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
        }
    }

    int FdRegistry::fd() const noexcept {
        return epollFd.get();
    }

    void FdRegistry::addPermanent(const int fd, const uint32_t events) {
        std::lock_guard lock(mutex);
        auto &state = states[fd];
        apply(fd, state.coros, state.permanentEvents | events);
        state.permanentEvents |= events;
    }

    void FdRegistry::add(const uint32_t events, const int fd, const std::coroutine_handle<> coro) {
        addAwaiter(events, fd, coro, std::nullopt);
    }

    void FdRegistry::addAwaiter(const uint32_t events,
                                const int fd,
                                const std::coroutine_handle<> coro,
                                const std::optional<int> cancellationFd) {
        std::lock_guard lock(mutex);
        if (!accepting) {
            throw std::runtime_error("Scheduler has been stopped");
        }
        if (cancellationFd) {
            addLocked(EPOLLIN, *cancellationFd, coro);
        }
        try {
            addLocked(events, fd, coro);
        } catch (...) {
            if (cancellationFd) {
                rollbackLocked(*cancellationFd, coro);
            }
            throw;
        }
    }

    void FdRegistry::addLocked(const uint32_t events, const int fd, const std::coroutine_handle<> coro) {
        const auto [it, inserted] = states.try_emplace(fd);
        auto &state = it->second;
        try {
            state.coros.push_back({.events = events, .handle = coro});
            try {
                apply(fd, state.coros, state.permanentEvents);
            } catch (...) {
                state.coros.pop_back();
                throw;
            }
        } catch (...) {
            if (inserted) {
                states.erase(it);
            }
            throw;
        }
    }

    void FdRegistry::remove(const int fd, const std::coroutine_handle<> coro) {
        std::lock_guard lock(mutex);
        const auto it = states.find(fd);
        if (it == states.end()) {
            return;
        }
        auto next = it->second.coros;
        const auto entry = std::ranges::find(next, coro, &Entry::handle);
        if (entry == next.end()) {
            return;
        }
        next.erase(entry);
        apply(fd, next, it->second.permanentEvents);
        if (next.empty() && it->second.permanentEvents == 0) {
            states.erase(it);
        } else {
            it->second.coros.swap(next);
        }
    }

    void FdRegistry::rollbackAdd(const int fd, const std::coroutine_handle<> coro) noexcept {
        std::lock_guard lock(mutex);
        rollbackLocked(fd, coro);
    }

    void FdRegistry::rollbackLocked(const int fd, const std::coroutine_handle<> coro) noexcept {
        const auto it = states.find(fd);
        if (it == states.end()) {
            return;
        }
        auto &state = it->second;
        const auto entry = std::ranges::find(state.coros, coro, &Entry::handle);
        if (entry == state.coros.end()) {
            return;
        }
        state.coros.erase(entry);
        try {
            apply(fd, state.coros, state.permanentEvents);
        } catch (...) {
            // In-memory registrations remain authoritative after a kernel error.
        }
        if (state.coros.empty() && state.permanentEvents == 0) {
            states.erase(it);
        }
    }

    void FdRegistry::forceRemoveFd(const int fd) noexcept {
        std::lock_guard lock(mutex);
        const auto it = states.find(fd);
        if (!active || it == states.end()) {
            return;
        }
        if (it->second.permanentEvents != 0) {
            it->second.coros.clear();
            try {
                apply(fd, it->second.coros, it->second.permanentEvents);
            } catch (...) {
            }
        } else {
            states.erase(it);
            while (epoll_ctl(epollFd.get(), EPOLL_CTL_DEL, fd, nullptr) < 0 && errno == EINTR) {
            }
        }
    }

    void FdRegistry::stopAccepting() noexcept {
        std::lock_guard lock(mutex);
        accepting = false;
    }

    void FdRegistry::deactivate() noexcept {
        std::lock_guard lock(mutex);
        accepting = false;
        active = false;
        states.clear();
        epollFd.reset(-1);
    }

    void FdRegistry::apply(const int fd, const std::vector<Entry> &coros, const uint32_t permanentEvents) const {
        uint32_t events = permanentEvents;
        for (const auto &entry: coros) {
            events |= entry.events;
        }
        if (events == 0) {
            while (epoll_ctl(epollFd.get(), EPOLL_CTL_DEL, fd, nullptr) < 0) {
                const int error = errno;
                if (error == EINTR) {
                    continue;
                }
                if (error != ENOENT && error != EBADF) {
                    throw std::system_error(error, std::system_category(), "Epoll del failed");
                }
                break;
            }
            return;
        }
        epoll_event event{.events = events, .data = {.fd = fd}};
        int operation = EPOLL_CTL_MOD;
        while (epoll_ctl(epollFd.get(), operation, fd, &event) < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (operation == EPOLL_CTL_MOD && error == ENOENT) {
                operation = EPOLL_CTL_ADD;
                continue;
            }
            throw std::system_error(error, std::system_category(), "Epoll add/mod failed");
        }
    }
} // namespace CoroLite
