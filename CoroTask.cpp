#include "CoroTask.h"

#include <array>
#include <cerrno>
#include <system_error>

#include "Consts.h"
#include "Logger.h"

namespace {
    thread_local CoroLite::EpollScheduler *currentWorkerScheduler = nullptr;
}

namespace CoroLite {
    void EpollScheduler::ThreadPool::enqueue(const std::coroutine_handle<> h) {
        {
            std::lock_guard lock(queueMutex);
            if (stopping) {
                throw std::logic_error("Cannot enqueue while thread pool is stopping");
            }
            queue.push_back(h);
        }
        cv.notify_one();
    }

    void EpollScheduler::ThreadPool::worker(const std::stop_token &st) {
        currentWorkerScheduler = &scheduler;
        while (true) {
            std::coroutine_handle<> h;
            {
                std::unique_lock lock(queueMutex);
                if (!cv.wait(lock, st, [&] { return stopping || !queue.empty(); })) {
                    return;
                }
                if (queue.empty()) {
                    return;
                }
                h = queue.front();
                queue.pop_front();
                ++activeWorkers;
            }
            const auto address = h.address();
            if (!h.done()) {
                h.resume();
            }
            scheduler.onResumeFinished(address);
            {
                std::lock_guard lock(queueMutex);
                --activeWorkers;
                if (waitingForIdle && activeWorkers == 0 && queue.empty()) {
                    idleCv.notify_all();
                }
            }
        }
    }

    void EpollScheduler::ThreadPool::waitIdle() {
        std::unique_lock lock(queueMutex);
        waitingForIdle = true;
        idleCv.wait(lock, [this] { return queue.empty() && activeWorkers == 0; });
        waitingForIdle = false;
    }

    void EpollScheduler::ThreadPool::drainAndStop() {
        {
            std::lock_guard lock(queueMutex);
            stopping = true;
        }
        cv.notify_all();
        for (auto &worker: workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        assert(queue.empty());
        log_v("ThreadPool: finished\n");
    }

    EpollScheduler::ThreadPool::ThreadPool(EpollScheduler &scheduler_, const size_t numThreads) :
        scheduler(scheduler_) {
        workers.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this](const auto &st) { worker(st); });
        }
    }

    EpollScheduler::EpollScheduler(const CancellationTokenSource &cts_) :
        cts(cts_) {
        registrations->addPermanent(wakeupSignal.getFd(), PollIn);
        registrations->addPermanent(cts.getToken().getFd(), PollIn);
        pendingResumes.reserve(Consts::EPOLL_BATCH_SIZE);
        threadPool = std::make_unique<ThreadPool>(*this, Consts::THREAD_POOL_SIZE);
    }

    EpollScheduler::~EpollScheduler() {
        if (currentWorkerScheduler == this) {
            log_e("EpollScheduler cannot be destroyed from its own worker\n");
            std::terminate();
        }
        std::unique_lock lock(lifecycleMutex);
        if (runActive && runThread == std::this_thread::get_id()) {
            log_e("EpollScheduler cannot be destroyed from its run() thread\n");
            std::terminate();
        }
        cts.requestStop();
        stopping.store(true);
        wakeupSignal.signal();
        if (runActive) {
            lifecycleCv.wait(lock, [this] { return !runActive; });
        }
        if (!cleanupComplete) {
            lock.unlock();
            shutdown();
        }
    }

    void EpollScheduler::beginDetachedStart() {
        std::lock_guard lock(lifecycleMutex);
        if (stopping.load()) {
            throw std::runtime_error("Scheduler has been stopped");
        }
        ++detachedStartCount;
    }

    void EpollScheduler::endDetachedStart() noexcept {
        std::lock_guard lock(lifecycleMutex);
        assert(detachedStartCount != 0);
        --detachedStartCount;
        lifecycleCv.notify_all();
    }

    void EpollScheduler::registerDetached(const std::coroutine_handle<> coro) {
        std::lock_guard lock(detachedMutex);
        if (stopping.load()) {
            throw std::runtime_error("Scheduler has been stopped");
        }
        detachedCoros.push_back(coro);
    }

    void EpollScheduler::unregisterDetached(const std::coroutine_handle<> coro) noexcept {
        std::lock_guard lock(detachedMutex);
        const auto it = std::ranges::find(detachedCoros, coro);
        if (it != detachedCoros.end()) {
            detachedCoros.erase(it);
        }
    }

    void EpollScheduler::add(const uint32_t events, const int fd, const std::coroutine_handle<> coro) const {
        addAwaiter(events, fd, coro, std::nullopt);
    }

    void EpollScheduler::addAwaiter(const uint32_t events,
                                    const int fd,
                                    const std::coroutine_handle<> coro,
                                    const std::optional<int> cancellationFd) const {
        if (stopping.load()) {
            throw std::runtime_error("Scheduler has been stopped");
        }
        registrations->addAwaiter(events, fd, coro, cancellationFd);
        wakeupSignal.signal();
    }

    void EpollScheduler::remove(const int fd, const std::coroutine_handle<> coro) const {
        registrations->remove(fd, coro);
    }

    void EpollScheduler::rollbackAdd(const int fd, const std::coroutine_handle<> coro) const noexcept {
        registrations->rollbackAdd(fd, coro);
    }

    void EpollScheduler::forceRemoveFd(const int fd) const noexcept {
        registrations->forceRemoveFd(fd);
    }

    void EpollScheduler::run() {
        {
            std::lock_guard lock(lifecycleMutex);
            if (runStarted || stopping.load()) {
                throw std::logic_error("run() is single-use");
            }
            runStarted = true;
            runActive = true;
            runThread = std::this_thread::get_id();
        }
        std::exception_ptr failure;
        try {
            std::array<epoll_event, Consts::EPOLL_BATCH_SIZE> events{};
            const auto cancellationToken = cts.getToken();
            while (!cancellationToken.isStopped()) {
                const int size =
                        epoll_wait(registrations->fd(), events.data(), events.size(), Consts::EPOLL_TIMEOUT_MS);
                if (size < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::system_error(errno, std::system_category(), "epoll_wait failed");
                }
                pendingResumes.clear();
                {
                    std::lock_guard lock(registrations->mutex);
                    for (int i = 0; i < size; ++i) {
                        const auto &event = events[static_cast<size_t>(i)];
                        const int fd = event.data.fd;
                        if (fd == wakeupSignal.getFd()) {
                            wakeupSignal.drain();
                            continue;
                        }
                        if (fd == cancellationToken.getFd()) {
                            continue;
                        }
                        if (const auto it = registrations->states.find(fd); it != registrations->states.end()) {
                            collectReadyCoroutines(it, event.events);
                        }
                    }
                }
                enqueuePendingResumes();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        shutdown();
        {
            // Last access to scheduler state: the destructor may proceed after this unlock.
            std::lock_guard lock(lifecycleMutex);
            runActive = false;
            lifecycleCv.notify_all();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void EpollScheduler::shutdown() noexcept {
        {
            std::lock_guard lock(lifecycleMutex);
            cts.requestStop();
            stopping.store(true);
        }
        registrations->stopAccepting();
        {
            std::unique_lock lock(lifecycleMutex);
            lifecycleCv.wait(lock, [this] { return detachedStartCount == 0; });
        }
        // No new waits can appear. Let running resumes finish before broadcasting
        // cancellation, so deduplication cannot strand a late cancellation waiter.
        threadPool->waitIdle();
        try {
            pendingResumes.clear();
            {
                std::lock_guard lock(registrations->mutex);
                if (const auto it = registrations->states.find(cts.getToken().getFd());
                    it != registrations->states.end()) {
                    collectReadyCoroutines(it, PollIn);
                }
            }
            enqueuePendingResumes();
        } catch (const std::exception &error) {
            log_e("Unable to resume cancellation waiters during shutdown: {}\n", error.what());
        }
        threadPool->drainAndStop();
        {
            std::lock_guard lock(registrations->mutex);
            pendingResumes.clear();
            scheduledCoros.clear();
            deferredResumes.clear();
        }
        registrations->deactivate();
        std::vector<std::coroutine_handle<>> suspended;
        {
            std::lock_guard lock(detachedMutex);
            suspended = std::move(detachedCoros);
        }
        for (const auto coro: suspended) {
            if (coro) {
                coro.destroy();
            }
        }
        timers->stop();
        {
            std::lock_guard lock(lifecycleMutex);
            cleanupComplete = true;
        }
    }

    void EpollScheduler::collectReadyCoroutines(const FdRegistry::States::iterator fdIt,
                                                const uint32_t occurredEvents) {
        const int fd = fdIt->first;
        const auto &state = fdIt->second;
        std::vector<FdRegistry::Entry> next;
        std::vector<std::coroutine_handle<>> ready;
        next.reserve(state.coros.size());
        ready.reserve(state.coros.size());
        const bool fatal = (occurredEvents & (PollErr | PollHUp)) != 0;
        try {
            for (const auto &entry: state.coros) {
                if ((entry.events & occurredEvents) != 0 || fatal) {
                    if (scheduledCoros.insert(entry.handle.address()).second) {
                        ready.push_back(entry.handle);
                        continue;
                    }
                    deferredResumes.insert(entry.handle.address());
                }
                next.push_back(entry);
            }
            pendingResumes.reserve(pendingResumes.size() + ready.size());
            registrations->apply(fd, next, state.permanentEvents);
        } catch (...) {
            for (const auto coro: ready) {
                scheduledCoros.erase(coro.address());
            }
            throw;
        }
        pendingResumes.insert(pendingResumes.end(), ready.begin(), ready.end());
        if (next.empty() && state.permanentEvents == 0) {
            registrations->states.erase(fdIt);
        } else {
            fdIt->second.coros.swap(next);
        }
    }

    void EpollScheduler::enqueuePendingResumes() const {
        for (const auto coro: pendingResumes) {
            threadPool->enqueue(coro);
        }
    }

    void EpollScheduler::onResumeFinished(void *const address) {
        std::lock_guard lock(registrations->mutex);
        scheduledCoros.erase(address);
        if (deferredResumes.erase(address) != 0) {
            wakeupSignal.signal();
        }
    }

    const CancellationTokenSource &EpollScheduler::getCancellationTokenSource() const {
        return cts;
    }

    TimerAwaiter::TimerAwaiter(const std::chrono::nanoseconds delay_, CancellationTokenOpt ct) :
        delay(delay_),
        cancellationToken(std::move(ct)) {
        if (delay.count() < 0) {
            throw std::invalid_argument("Timer delay must not be negative");
        }
    }

    bool TimerAwaiter::await_ready() const noexcept {
        return delay.count() == 0;
    }

    void TimerAwaiter::await_suspend(const std::coroutine_handle<> h) {
        if (cancellationToken && cancellationToken->isStopped()) {
            throw CancellationTokenException{};
        }
        const auto *const scheduler = getScheduler();
        timer = scheduler->timers->acquire();
        timer.get().arm(delay);
        handle = h;
        const auto cancellationFd = cancellationToken ? std::optional{cancellationToken->getFd()} : std::nullopt;
        try {
            scheduler->addAwaiter(EpollScheduler::PollIn, timer.get().getFd(), h, cancellationFd);
        } catch (...) {
            if (cancellationToken && cancellationToken->isStopped()) {
                throw CancellationTokenException{};
            }
            throw;
        }
    }

    void TimerAwaiter::await_resume() {
        if (handle) {
            const auto *const scheduler = getScheduler();
            scheduler->rollbackAdd(timer.get().getFd(), handle);
            if (cancellationToken) {
                scheduler->rollbackAdd(cancellationToken->getFd(), handle);
            }
            timer.consume();
            timer.reset();
        }
        if (cancellationToken && cancellationToken->isStopped()) {
            throw CancellationTokenException{};
        }
    }
} // namespace CoroLite
