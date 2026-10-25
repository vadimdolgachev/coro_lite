#include "TimerPool.h"

#include <utility>

namespace CoroLite {
    TimerLease::TimerLease(std::shared_ptr<TimerPool> pool_, std::unique_ptr<Timer> timer_) noexcept :
        pool(std::move(pool_)),
        timer(std::move(timer_)) {
    }

    TimerLease::TimerLease(TimerLease &&other) noexcept :
        pool(std::move(other.pool)),
        timer(std::move(other.timer)),
        expired(other.expired) {
    }

    TimerLease &TimerLease::operator=(TimerLease &&other) noexcept {
        if (this != &other) {
            reset();
            pool = std::move(other.pool);
            timer = std::move(other.timer);
            expired = other.expired;
        }
        return *this;
    }

    TimerLease::~TimerLease() {
        reset();
    }

    const Timer &TimerLease::get() const noexcept {
        return *timer;
    }

    TimerLease::operator bool() const noexcept {
        return timer != nullptr;
    }

    void TimerLease::consume() noexcept {
        expired = timer->consume();
    }

    void TimerLease::reset() noexcept {
        if (timer) {
            pool->release(std::move(timer), expired);
        }
        pool.reset();
        expired = false;
    }

    TimerLease TimerPool::acquire() {
        std::unique_ptr<Timer> timer;
        {
            std::lock_guard lock(mutex);
            if (stopped) {
                throw std::runtime_error("Timer pool has been stopped");
            }
            if (!idle.empty()) {
                timer = std::move(idle.back());
                idle.pop_back();
            }
        }
        if (!timer) {
            timer = std::make_unique<Timer>();
        }
        return {shared_from_this(), std::move(timer)};
    }

    void TimerPool::release(std::unique_ptr<Timer> timer, const bool expired) noexcept {
        if (!expired && !timer->reset()) {
            return;
        }
        std::lock_guard lock(mutex);
        if (!stopped && idle.size() < Consts::MAX_CACHED_TIMERS) {
            try {
                idle.push_back(std::move(timer));
            } catch (...) {
                // Allocation failure drops the timer instead of escaping a destructor.
            }
        }
    }

    void TimerPool::stop() noexcept {
        std::lock_guard lock(mutex);
        stopped = true;
        idle.clear();
    }
} // namespace CoroLite
