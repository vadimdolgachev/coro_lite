#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "CancellationToken.h"
#include "CoroTask.h"

using namespace CoroLite;

namespace {
    struct Lifetime final {
        explicit Lifetime(std::atomic_int &destructions_) : destructions(destructions_) {}
        ~Lifetime() { destructions.fetch_add(1); }
        std::atomic_int &destructions;
    };

    CoroTask<> complete([[maybe_unused]] std::shared_ptr<Lifetime> lifetime = {}) { co_return; }

    CoroTask<> suspend([[maybe_unused]] std::shared_ptr<Lifetime> lifetime) {
        co_await std::suspend_always{};
    }

    CoroTask<> suspendNested([[maybe_unused]] std::shared_ptr<Lifetime> lifetime) {
        co_await suspend(std::move(lifetime));
    }

    CoroTask<> awaitTask(CoroTask<> task) { co_await std::move(task); }

    CoroTask<> waitForCancellation(std::atomic_int &completions) {
        try {
            co_await TimerAwaiter{std::chrono::hours(1), co_await GetCancellationToken{}};
        } catch (const CancellationTokenException &) {
            completions.fetch_add(1);
        }
    }

    template<typename F>
    bool throwsLogicError(F &&f) {
        try {
            std::forward<F>(f)();
        } catch (const std::logic_error &) {
            return true;
        }
        return false;
    }
}

int main() {
    std::atomic_int completedDestructions = 0;
    {
        CancellationTokenSource cts;
        EpollScheduler scheduler(cts);
        auto task = complete(std::make_shared<Lifetime>(completedDestructions));
        task.startDetached(scheduler);
        if (completedDestructions.load() != 1) return 1;
    }
    if (completedDestructions.load() != 1) return 2;

    std::atomic_int suspendedDestructions = 0;
    {
        CancellationTokenSource cts;
        EpollScheduler scheduler(cts);
        auto task = suspendNested(std::make_shared<Lifetime>(suspendedDestructions));
        task.startDetached(scheduler);
    }
    if (suspendedDestructions.load() != 1) return 3;

    {
        CancellationTokenSource cts;
        EpollScheduler scheduler(cts);
        auto task = complete();
        task.start(scheduler);
        if (!throwsLogicError([&] { task.start(scheduler); })) return 4;
        auto parent = awaitTask(std::move(task));
        if (!throwsLogicError([&] { parent.start(scheduler); })) return 5;
    }

    {
        auto task = complete();
        [[maybe_unused]] auto awaiter = std::move(task).operator co_await();
        if (!throwsLogicError([&] { std::move(task).operator co_await(); })) return 6;
    }

    {
        constexpr size_t taskCount = 16;
        CancellationTokenSource cts;
        EpollScheduler scheduler(cts);
        std::atomic_int completions = 0;
        std::vector<CoroTask<>> roots;
        roots.reserve(taskCount);
        for (size_t i = 0; i < taskCount; ++i) {
            roots.emplace_back(waitForCancellation(completions));
            roots.back().start(scheduler);
        }
        cts.requestStop();
        scheduler.run();
        if (completions.load() != taskCount) return 7;
    }
    return 0;
}
