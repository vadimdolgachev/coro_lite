#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <latch>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "TestCheck.h"
#include "CoroTask.h"
#include "Socket.h"

using namespace CoroLite;
using namespace std::chrono_literals;

namespace {
    std::atomic_int recvFd{-1}, recvError{0}, sendFd{-1}, sendError{0};
    std::atomic_bool discardOnAgain{false};
    std::atomic_int timerCreates{0}, armError{0}, writes{0};
    std::atomic_int watchFd{-1}, watchOperation{EPOLL_CTL_DEL};
    std::atomic_bool observed{false};
}

extern "C" ssize_t __real_recv(int, void *, size_t, int);
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" int __real_timerfd_create(int, int);
extern "C" int __real_timerfd_settime(int, int, const itimerspec *, itimerspec *);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);

extern "C" ssize_t __wrap_recv(const int fd, void *buffer, const size_t size, const int flags) {
    if (fd == recvFd.load()) {
        const int error = recvError.load();
        if ((error == EINTR || (flags & MSG_PEEK) == 0) && recvError.exchange(0) != 0) {
            if (error == EAGAIN && discardOnAgain.exchange(false)) {
                unsigned char ignored;
                (void)__real_recv(fd, &ignored, 1, MSG_DONTWAIT);
            }
            errno = error;
            return -1;
        }
    }
    return __real_recv(fd, buffer, size, flags);
}

extern "C" ssize_t __wrap_send(const int fd, const void *buffer, const size_t size, const int flags) {
    if (fd == sendFd.load()) {
        if (const int error = sendError.exchange(0); error != 0) {
            errno = error;
            return -1;
        }
    }
    return __real_send(fd, buffer, size, flags);
}

extern "C" int __wrap_timerfd_create(const int clock, const int flags) {
    ++timerCreates;
    return __real_timerfd_create(clock, flags);
}

extern "C" int __wrap_timerfd_settime(const int fd, const int flags, const itimerspec *spec, itimerspec *old) {
    if (spec->it_value.tv_sec != 0 || spec->it_value.tv_nsec != 0) {
        if (const int error = armError.exchange(0); error != 0) {
            errno = error;
            return -1;
        }
    }
    return __real_timerfd_settime(fd, flags, spec, old);
}

extern "C" ssize_t __wrap_write(const int fd, const void *buffer, const size_t size) {
    ++writes;
    return __real_write(fd, buffer, size);
}

extern "C" int __wrap_epoll_ctl(const int epoll, const int operation, const int fd, epoll_event *event) {
    const int result = __real_epoll_ctl(epoll, operation, fd, event);
    if (result == 0 && fd == watchFd.load() && operation == watchOperation.load()) observed.store(true);
    return result;
}

namespace {
    struct Pair final {
        UniqueFd peer;
        SocketPtr socket;
        Pair() {
            int fds[2];
            if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
                throw std::system_error(errno, std::system_category(), "socketpair failed");
            }
            peer.reset(fds[1]);
            socket = std::make_shared<Socket>(fds[0]);
        }
        void send(const char value) const { check(__real_send(peer.get(), &value, 1, MSG_NOSIGNAL) == 1); }
    };

    void waitObserved() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!observed.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        check(observed.load());
        watchFd = -1;
    }

    struct SignalAwaiter final : SchedulerAware<EpollScheduler> {
        const CompletionSignal &signal;
        explicit SignalAwaiter(const CompletionSignal &signal_) : signal(signal_) {}
        static bool await_ready() noexcept { return false; }
        void await_suspend(const std::coroutine_handle<> h) const { getScheduler()->add(EpollScheduler::PollIn, signal.getFd(), h); }
        void await_resume() const noexcept { signal.drain(); }
    };

    CoroTask<> lateTimers(CancellationTokenSource &cts, std::atomic_bool &completed) {
        co_await TimerAwaiter{1ms, std::nullopt};
        try {
            for (int i = 0; i < 10; ++i) co_await TimerAwaiter{1ms, cts.getToken()};
            completed = true;
        } catch (...) {}
        cts.requestStop();
    }

    CoroTask<> activeRunner(std::latch &entered, std::atomic_int &completed) {
        co_await TimerAwaiter{1ms, std::nullopt};
        entered.count_down();
        try { co_await TimerAwaiter{1h, co_await GetCancellationToken{}}; }
        catch (const CancellationTokenException &) { ++completed; }
    }

    struct Lifetime final {
        std::atomic_int &destroyed;
        explicit Lifetime(std::atomic_int &count) : destroyed(count) {}
        ~Lifetime() { ++destroyed; }
    };

    struct PausedStart final {
        std::latch &entered;
        std::latch &release;
        int touched = 0;
        static bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) {
            entered.count_down();
            release.wait();
            touched = 1;
        }
        static void await_resume() noexcept {}
    };

    CoroTask<> starting(std::shared_ptr<Lifetime> lifetime, std::latch &entered, std::latch &release) {
        check(lifetime != nullptr);
        co_await PausedStart{entered, release};
    }

    CoroTask<> blocker(const CompletionSignal &signal, std::latch &entered, const std::latch &release) {
        co_await SignalAwaiter{signal};
        entered.count_down();
        release.wait();
    }

    CoroTask<> ordered(const CompletionSignal &signal, std::atomic_int &sequence, std::atomic_int &result) {
        co_await SignalAwaiter{signal};
        result = sequence.fetch_add(1);
    }

    void fifo() {
        CancellationTokenSource cts;
        EpollScheduler scheduler(cts);
        std::array<CompletionSignal, Consts::THREAD_POOL_SIZE> signals;
        std::vector<std::unique_ptr<std::latch>> releases;
        std::latch entered(Consts::THREAD_POOL_SIZE);
        for (const auto &signal : signals) {
            releases.push_back(std::make_unique<std::latch>(1));
            blocker(signal, entered, *releases.back()).startDetached(scheduler);
            signal.signal();
        }
        std::thread runner([&] { scheduler.run(); });
        entered.wait();
        CompletionSignal oldSignal, newSignal;
        std::atomic_int sequence{0}, older{-1}, newer{-1};
        ordered(oldSignal, sequence, older).startDetached(scheduler);
        observed = false;
        watchOperation = EPOLL_CTL_DEL;
        watchFd = oldSignal.getFd();
        oldSignal.signal();
        waitObserved();
        ordered(newSignal, sequence, newer).startDetached(scheduler);
        observed = false;
        watchFd = newSignal.getFd();
        newSignal.signal();
        waitObserved();
        releases.front()->count_down();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (sequence.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        check(older == 0 && newer == 1);
        for (size_t i = 1; i < releases.size(); ++i) releases[i]->count_down();
        cts.requestStop();
        runner.join();
    }

    struct DeferredAwaiter final : SchedulerAware<EpollScheduler> {
        const CompletionSignal &signal;
        std::latch &registered;
        std::latch &release;
        DeferredAwaiter(const CompletionSignal &s, std::latch &r, std::latch &exit) : signal(s), registered(r), release(exit) {}
        static bool await_ready() noexcept { return false; }
        void await_suspend(const std::coroutine_handle<> h) const {
            getScheduler()->add(EpollScheduler::PollIn, signal.getFd(), h);
            registered.count_down();
            release.wait();
        }
        void await_resume() const noexcept { signal.drain(); }
    };

    CoroTask<> deferred(const CompletionSignal &signal, std::latch &registered, std::latch &release,
                        CancellationTokenSource &cts, std::atomic_bool &completed) {
        co_await SignalAwaiter{signal};
        co_await DeferredAwaiter{signal, registered, release};
        completed = true;
        cts.requestStop();
    }

    void schedulerTests() {
        {
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            std::atomic_bool completed{false};
            lateTimers(cts, completed).startDetached(scheduler);
            scheduler.run();
            check(completed);
        }
        for (int i = 0; i < 30; ++i) {
            CancellationTokenSource cts;
            auto scheduler = std::make_unique<EpollScheduler>(cts);
            std::latch entered(1);
            std::atomic_int completed{0};
            activeRunner(entered, completed).startDetached(*scheduler);
            std::thread runner([raw = scheduler.get()] { raw->run(); });
            entered.wait();
            scheduler.reset();
            runner.join();
            check(completed == 1);
        }
        for (int i = 0; i < 30; ++i) {
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            std::latch entered(1), release(1);
            std::atomic_int destroyed{0};
            std::thread starter([&] { starting(std::make_shared<Lifetime>(destroyed), entered, release).startDetached(scheduler); });
            entered.wait();
            cts.requestStop();
            std::thread runner([&] { scheduler.run(); });
            release.count_down();
            starter.join();
            runner.join();
            check(destroyed == 1);
        }
        fifo();
        {
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            CompletionSignal signal;
            std::latch registered(1), release(1);
            std::atomic_bool completed{false};
            deferred(signal, registered, release, cts, completed).startDetached(scheduler);
            std::thread runner([&] { scheduler.run(); });
            signal.signal();
            registered.wait();
            observed = false;
            watchFd = signal.getFd();
            watchOperation = EPOLL_CTL_MOD;
            signal.signal();
            waitObserved();
            release.count_down();
            runner.join();
            check(completed);
        }
    }

    CoroTask<> reader(SocketPtr socket, CancellationToken token, CancellationTokenSource &cts,
                      std::atomic_int &result) {
        std::array<unsigned char, 1> buffer{};
        try {
            const auto count = co_await socket->read(buffer, std::move(token));
            result = count == 1 && buffer[0] == 'y' ? 1 : -1;
        } catch (const CancellationTokenException &) { result = 2; }
        catch (...) { result = -2; }
        cts.requestStop();
    }

    CoroTask<> writer(SocketPtr socket, CancellationTokenSource &cts, std::atomic_bool &completed) {
        std::array<unsigned char, 1> buffer{'z'};
        try { completed = co_await socket->write(buffer, std::nullopt) == 1; }
        catch (...) {}
        cts.requestStop();
    }

    void socketTests() {
        for (const bool cancel : {false, true}) {
            Pair pair;
            CancellationTokenSource cts, readerCts;
            EpollScheduler scheduler(cts);
            std::atomic_int result{0};
            pair.send('x');
            recvFd = pair.socket->fd();
            recvError = EAGAIN;
            discardOnAgain = true;
            reader(pair.socket, readerCts.getToken(), cts, result).startDetached(scheduler);
            check(recvError == 0 && result == 0);
            if (cancel) readerCts.requestStop();
            else pair.send('y');
            scheduler.run();
            check(result == (cancel ? 2 : 1));
            recvFd = -1;
        }
        {
            Pair pair;
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            sendFd = pair.socket->fd();
            sendError = EAGAIN;
            std::atomic_bool completed{false};
            writer(pair.socket, cts, completed).startDetached(scheduler);
            scheduler.run();
            check(completed && sendError == 0);
            sendFd = -1;
        }
        {
            Pair pair;
            pair.send('y');
            recvFd = pair.socket->fd();
            recvError = EINTR;
            check(!pair.socket->isEof() && recvError == 0);
            recvFd = -1;
        }
        {
            Pair pair;
            std::atomic_int result{0};
            {
                CancellationTokenSource cts;
                EpollScheduler scheduler(cts);
                reader(pair.socket, cts.getToken(), cts, result).startDetached(scheduler);
                cts.requestStop();
                scheduler.run();
            }
            check(result == 2);
            pair.socket->close();
        }
        {
            Pair pair;
            const auto weak = std::weak_ptr(pair.socket);
            {
                std::array<unsigned char, 1> buffer{};
                auto task = pair.socket->read(buffer, std::nullopt);
                pair.socket.reset();
                check(!weak.expired());
            }
            check(weak.expired());
        }
    }

    CoroTask<> timerLoop(CancellationTokenSource &cts, std::atomic_bool &completed) {
        try {
            for (int i = 0; i < 20; ++i) co_await TimerAwaiter{5ms, std::nullopt};
            completed = true;
        } catch (...) {}
        cts.requestStop();
    }

    CoroTask<> armFailure(CancellationTokenSource &cts, std::atomic_bool &completed) {
        armError = EIO;
        try { co_await TimerAwaiter{1ms, cts.getToken()}; }
        catch (const std::system_error &error) { completed = error.code().value() == EIO; }
        armError = EINTR;
        co_await TimerAwaiter{1ms, cts.getToken()};
        cts.requestStop();
    }

    void timerTests() {
        {
            CancellationTokenSource cts;
            const auto first = cts.getToken();
            const auto second = cts.getToken();
            cts.requestStop();
            first.drain();
            pollfd ready{.fd = second.getFd(), .events = POLLIN, .revents = 0};
            check(poll(&ready, 1, 0) == 1);
        }
        {
            CancellationTokenSource cts;
            FdRegistry registry;
            bool rejected = false;
            try { registry.addAwaiter(EPOLLIN, -1, std::noop_coroutine(), cts.getToken().getFd()); }
            catch (const std::system_error &) { rejected = true; }
            cts.requestStop();
            epoll_event event{};
            check(rejected && epoll_wait(registry.fd(), &event, 1, 0) == 0);
        }

        {
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            const int before = timerCreates;
            const TimerAwaiter immediate{0ns, std::nullopt};
            check(immediate.await_ready() && timerCreates == before);
            bool rejected = false;
            try { (void)TimerAwaiter{-1ns, std::nullopt}; }
            catch (const std::invalid_argument &) { rejected = true; }
            check(rejected);
        }
        {
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            std::atomic_bool completed{false};
            const int before = timerCreates;
            const int beforeWrites = writes;
            timerLoop(cts, completed).startDetached(scheduler);
            scheduler.run();
            check(completed && timerCreates - before == 1);
            check(writes - beforeWrites < 40);
            std::printf("20 sequential waits: timerfd_create=%d, eventfd writes=%d\n",
                        timerCreates.load() - before, writes.load() - beforeWrites);
        }
        {
            CancellationTokenSource cts;
            EpollScheduler scheduler(cts);
            std::atomic_bool completed{false};
            armFailure(cts, completed).startDetached(scheduler);
            scheduler.run();
            check(completed && armError == 0);
        }
        {
            const auto pool = std::make_shared<TimerPool>();
            std::vector<TimerLease> leases;
            for (size_t i = 0; i < Consts::MAX_CACHED_TIMERS + 1; ++i) leases.push_back(pool->acquire());
            leases.clear();
            const int before = timerCreates;
            for (size_t i = 0; i < Consts::MAX_CACHED_TIMERS + 1; ++i) leases.push_back(pool->acquire());
            check(timerCreates - before == 1);
            pool->stop();
            const int fd = leases.front().get().getFd();
            leases.clear();
            check(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        }
        {
            const auto pool = std::make_shared<TimerPool>();
            auto lease = pool->acquire();
            const int fd = lease.get().getFd();
            lease.get().arm(1ms);
            pollfd ready{.fd = fd, .events = POLLIN, .revents = 0};
            check(poll(&ready, 1, 1000) == 1);
            lease.reset();
            lease = pool->acquire();
            check(lease.get().getFd() == fd);
            check(poll(&ready, 1, 0) == 0);
        }
    }
}

int main(const int argc, char **argv) {
    check(argc == 2);
    const std::string_view mode(argv[1]);
    if (mode == "scheduler") schedulerTests();
    else if (mode == "socket") socketTests();
    else if (mode == "timer") timerTests();
    else return 2;
}
