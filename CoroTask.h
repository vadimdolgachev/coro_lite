#ifndef CORO_LITE_COROTASK_H
#define CORO_LITE_COROTASK_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/epoll.h>

#include "CancellationToken.h"
#include "CompletionSignal.h"
#include "FdRegistry.h"
#include "TimerPool.h"

namespace CoroLite {
    class EpollScheduler final {
        friend class Socket;
        friend struct TimerAwaiter;

        class ThreadPool final {
            EpollScheduler &scheduler;
            std::mutex queueMutex;
            std::condition_variable_any cv;
            std::condition_variable idleCv;
            std::deque<std::coroutine_handle<>> queue;
            std::vector<std::jthread> workers;
            bool stopping = false;
            size_t activeWorkers = 0;
            bool waitingForIdle = false;

        public:
            void enqueue(std::coroutine_handle<> h);
            void worker(const std::stop_token &st);
            void drainAndStop();
            void waitIdle();
            ThreadPool(EpollScheduler &scheduler_, size_t numThreads);
        };

    public:
        using PollEvents = uint32_t;
        static constexpr PollEvents PollIn = EPOLLIN;
        static constexpr PollEvents PollOut = EPOLLOUT;
        static constexpr PollEvents PollErr = EPOLLERR;
        static constexpr PollEvents PollHUp = EPOLLHUP;
        static constexpr PollEvents PollRdHUp = EPOLLRDHUP;

        explicit EpollScheduler(const CancellationTokenSource &cts_);
        // May wait for run() on another thread; destruction from a worker is forbidden.
        ~EpollScheduler();
        EpollScheduler(const EpollScheduler &) = delete;
        EpollScheduler &operator=(const EpollScheduler &) = delete;

        void add(uint32_t events, int fd, std::coroutine_handle<> coro) const;
        void addAwaiter(uint32_t events, int fd, std::coroutine_handle<> coro, std::optional<int> cancellationFd) const;
        void remove(int fd, std::coroutine_handle<> coro) const;
        void rollbackAdd(int fd, std::coroutine_handle<> coro) const noexcept;
        void forceRemoveFd(int fd) const noexcept;
        void run();
        void registerDetached(std::coroutine_handle<> coro);
        void unregisterDetached(std::coroutine_handle<> coro) noexcept;
        void beginDetachedStart();
        void endDetachedStart() noexcept;
        [[nodiscard]] const CancellationTokenSource &getCancellationTokenSource() const;

    private:
        void collectReadyCoroutines(FdRegistry::States::iterator fdIt, uint32_t occurredEvents);
        void enqueuePendingResumes() const;
        void shutdown() noexcept;
        void onResumeFinished(void *address);

        std::shared_ptr<FdRegistry> registrations = std::make_shared<FdRegistry>();
        CompletionSignal wakeupSignal;
        std::shared_ptr<TimerPool> timers = std::make_shared<TimerPool>();
        std::vector<std::coroutine_handle<>> pendingResumes;
        std::unordered_set<void *> scheduledCoros;
        std::unordered_set<void *> deferredResumes;
        std::mutex detachedMutex;
        std::vector<std::coroutine_handle<>> detachedCoros;
        std::mutex lifecycleMutex;
        std::condition_variable lifecycleCv;
        size_t detachedStartCount = 0;
        bool runStarted = false;
        bool runActive = false;
        bool cleanupComplete = false;
        std::thread::id runThread;
        std::atomic_bool stopping = false;
        CancellationTokenSource cts;
        std::unique_ptr<ThreadPool> threadPool;
    };

    template<typename T>
    struct SchedulerAware {
        void setScheduler(T *const s) noexcept {
            sched = s;
        }

        [[nodiscard]] T *getScheduler() const noexcept {
            return sched;
        }

    private:
        T *sched = nullptr;
    };

    struct TimerAwaiter final : SchedulerAware<EpollScheduler> {
        explicit TimerAwaiter(std::chrono::nanoseconds delay_, CancellationTokenOpt ct);
        [[nodiscard]] bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> h);
        void await_resume();

    private:
        std::chrono::nanoseconds delay;
        TimerLease timer;
        CancellationTokenOpt cancellationToken;
        std::coroutine_handle<> handle;
    };

    enum class CoroLifecycle {
        Created,
        Started,
        Awaited,
        Detached,
        Completed,
    };

    template<typename PromiseType>
    struct PromiseFinalAwaiter final {
        [[nodiscard]] constexpr bool await_ready() const noexcept {
            return false;
        }

        std::coroutine_handle<> await_suspend(const std::coroutine_handle<PromiseType> coro) noexcept {
            auto &promise = coro.promise();
            const auto previous = promise.lifecycle.exchange(CoroLifecycle::Completed);
            const auto continuation = promise.continuation;
            if (previous == CoroLifecycle::Detached) {
                if (auto *const scheduler = promise.getScheduler()) {
                    scheduler->unregisterDetached(coro);
                }
                coro.destroy();
                return std::noop_coroutine();
            }
            return continuation != nullptr ? continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {
        }
    };

    template<typename PromiseType>
    struct PromiseBase : SchedulerAware<EpollScheduler> {
        auto initial_suspend() const noexcept {
            return std::suspend_always{};
        }

        auto final_suspend() const noexcept {
            return PromiseFinalAwaiter<PromiseType>{};
        }

        void unhandled_exception() {
            exception = std::current_exception();
        }

        template<typename T>
        decltype(auto) await_transform(T &&a) {
            if constexpr (requires { a.setScheduler(this->getScheduler()); })
                a.setScheduler(this->getScheduler());
            return std::forward<T>(a);
        }

        std::exception_ptr exception;
        std::coroutine_handle<> continuation = nullptr;
        std::atomic<CoroLifecycle> lifecycle = CoroLifecycle::Created;
    };

    template<typename PromiseType>
    struct PromiseVoid : PromiseBase<PromiseType> {
        void return_void() const noexcept {
        }
    };

    template<typename T, typename Base>
    struct PromiseValue : PromiseBase<Base> {
        std::optional<T> value;

        template<typename U>
        void return_value(U &&v) {
            value.emplace(std::forward<U>(v));
        }
    };

    struct GetScheduler final : SchedulerAware<EpollScheduler> {
        [[nodiscard]] bool await_ready() const noexcept {
            return true;
        }

        void await_suspend(std::coroutine_handle<>) noexcept {
        }

        [[nodiscard]] EpollScheduler *await_resume() const noexcept {
            return this->getScheduler();
        }
    };

    struct GetCancellationToken final : SchedulerAware<EpollScheduler> {
        [[nodiscard]] bool await_ready() const noexcept {
            return true;
        }

        void await_suspend(std::coroutine_handle<>) noexcept {
        }

        [[nodiscard]] CancellationToken await_resume() const noexcept {
            return getScheduler()->getCancellationTokenSource().getToken();
        }
    };

    template<typename T = void>
    class CoroTask final {
    public:
        template<typename PromiseType>
        using PromiseBase =
                std::conditional_t<std::is_void_v<T>, PromiseVoid<PromiseType>, PromiseValue<T, PromiseType>>;

        struct promise_type final : PromiseBase<promise_type> {
            CoroTask get_return_object() {
                return CoroTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
        };

        struct CoroTaskAwaiter final {
            explicit CoroTaskAwaiter(CoroTask &&task_) noexcept :
                task(std::move(task_)) {
            }

            CoroTaskAwaiter(CoroTaskAwaiter &) = delete;

            auto await_resume() {
                auto &promise = task.handle.promise();
                if (promise.exception) {
                    std::rethrow_exception(promise.exception);
                }
                if constexpr (!std::is_void_v<T>) {
                    return std::move(*promise.value);
                }
            }

            bool await_ready() noexcept {
                return task.handle.done();
            }

            template<typename ParentPromiseT>
            std::coroutine_handle<> await_suspend(const std::coroutine_handle<ParentPromiseT> parent) noexcept {
                task.handle.promise().continuation = parent;
                return task.handle;
            }

            CoroTask task;
        };

        explicit CoroTask(const std::coroutine_handle<promise_type> h) :
            handle(h) {
        }

        CoroTask(CoroTask &&other) noexcept :
            handle(std::exchange(other.handle, nullptr)) {
        }

        CoroTask &operator=(CoroTask &&other) noexcept = delete;

        CoroTask(CoroTask &) = delete;

        ~CoroTask() {
            if (handle == nullptr) {
                return;
            }
            const auto lifecycle = handle.promise().lifecycle.load();
            if (lifecycle == CoroLifecycle::Started && !handle.done()) {
                std::terminate();
            }
            if (lifecycle != CoroLifecycle::Detached) {
                handle.destroy();
            }
        }

        void start(EpollScheduler &scheduler) {
            transition(CoroLifecycle::Created, CoroLifecycle::Started, "start");
            handle.promise().setScheduler(&scheduler);
            handle.resume();
            if (handle.done()) {
                if (const auto exception = handle.promise().exception) {
                    std::rethrow_exception(exception);
                }
            }
        }

        void startDetached(EpollScheduler &scheduler) {
            scheduler.beginDetachedStart();
            struct StartGuard final {
                EpollScheduler &owner;
                ~StartGuard() {
                    owner.endDetachedStart();
                }
            } guard{scheduler};
            transition(CoroLifecycle::Created, CoroLifecycle::Detached, "startDetached");
            handle.promise().setScheduler(&scheduler);
            const auto coro = std::exchange(handle, nullptr);
            try {
                scheduler.registerDetached(coro);
                coro.resume();
            } catch (...) {
                scheduler.unregisterDetached(coro);
                coro.destroy();
                throw;
            }
        }

        auto operator co_await() && {
            transition(CoroLifecycle::Created, CoroLifecycle::Awaited, "co_await");
            return CoroTaskAwaiter{std::move(*this)};
        }

        void setScheduler(EpollScheduler *s) noexcept {
            if (handle != nullptr) {
                handle.promise().setScheduler(s);
            }
        }

    private:
        void transition(CoroLifecycle expected, const CoroLifecycle desired, const char *operation) {
            if (handle == nullptr) {
                throw std::logic_error(std::string(operation) + ": empty coroutine task");
            }
            if (!handle.promise().lifecycle.compare_exchange_strong(expected, desired)) {
                throw std::logic_error(std::string(operation) + ": coroutine task already consumed");
            }
        }

        std::coroutine_handle<promise_type> handle;
    };
} // namespace CoroLite

#endif // CORO_LITE_COROTASK_H
