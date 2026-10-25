#ifndef CORO_LITE_FDREGISTRY_H
#define CORO_LITE_FDREGISTRY_H

#include <coroutine>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "FdUtils.h"

namespace CoroLite {
    // Shared independently of the scheduler; sockets keep only a weak reference.
    class FdRegistry final {
        friend class EpollScheduler;

        struct Entry final {
            uint32_t events{};
            std::coroutine_handle<> handle;
        };

        struct State final {
            uint32_t permanentEvents = 0;
            std::vector<Entry> coros;
        };

        using States = std::unordered_map<int, State>;

    public:
        FdRegistry();
        void addPermanent(int fd, uint32_t events);
        void add(uint32_t events, int fd, std::coroutine_handle<> coro);
        void addAwaiter(uint32_t events, int fd, std::coroutine_handle<> coro, std::optional<int> cancellationFd);
        void remove(int fd, std::coroutine_handle<> coro);
        void rollbackAdd(int fd, std::coroutine_handle<> coro) noexcept;
        void forceRemoveFd(int fd) noexcept;
        void stopAccepting() noexcept;
        void deactivate() noexcept;
        [[nodiscard]] int fd() const noexcept;

    private:
        void addLocked(uint32_t events, int fd, std::coroutine_handle<> coro);
        void rollbackLocked(int fd, std::coroutine_handle<> coro) noexcept;
        void apply(int fd, const std::vector<Entry> &coros, uint32_t permanentEvents) const;

        std::mutex mutex;
        UniqueFd epollFd;
        States states;
        bool accepting = true;
        bool active = true;
    };
} // namespace CoroLite

#endif // CORO_LITE_FDREGISTRY_H
