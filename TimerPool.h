#ifndef CORO_LITE_TIMERPOOL_H
#define CORO_LITE_TIMERPOOL_H

#include <memory>
#include <mutex>
#include <vector>

#include "Timer.h"

namespace CoroLite {
    class TimerPool;

    class TimerLease final {
        friend class TimerPool;

    public:
        TimerLease() = default;
        TimerLease(const TimerLease &) = delete;
        TimerLease &operator=(const TimerLease &) = delete;
        TimerLease(TimerLease &&other) noexcept;
        TimerLease &operator=(TimerLease &&other) noexcept;
        ~TimerLease();
        [[nodiscard]] const Timer &get() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
        void consume() noexcept;
        void reset() noexcept;

    private:
        TimerLease(std::shared_ptr<TimerPool> pool_, std::unique_ptr<Timer> timer_) noexcept;
        std::shared_ptr<TimerPool> pool;
        std::unique_ptr<Timer> timer;
        bool expired = false;
    };

    class TimerPool final : public std::enable_shared_from_this<TimerPool> {
        friend class TimerLease;

    public:
        [[nodiscard]] TimerLease acquire();
        void stop() noexcept;

    private:
        void release(std::unique_ptr<Timer> timer, bool expired) noexcept;
        std::mutex mutex;
        std::vector<std::unique_ptr<Timer>> idle;
        bool stopped = false;
    };
} // namespace CoroLite

#endif // CORO_LITE_TIMERPOOL_H
