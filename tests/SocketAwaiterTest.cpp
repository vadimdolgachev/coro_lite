#include <array>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

#include "CancellationToken.h"
#include "CoroTask.h"
#include "Socket.h"

using namespace CoroLite;

namespace {
    CoroTask<> cancelledReader(const SocketPtr &socket, CancellationToken token,
                               std::atomic_bool &cancelled) {
        std::array<unsigned char, 16> buf{};
        try {
            co_await socket->read(buf, std::move(token));
        } catch (const CancellationTokenException &) {
            cancelled.store(true);
        }
    }

    CoroTask<> survivingReader(const SocketPtr &socket, std::atomic_bool &received) {
        std::array<unsigned char, 16> buf{};
        const size_t bytes = co_await socket->read(buf, std::nullopt);
        received.store(bytes == 1 && buf[0] == 'x');
    }

    CoroTask<> driver(const int peerFd, const std::atomic_bool &cancelled,
                      const std::atomic_bool &received, CancellationTokenSource &stop) {
        for (int i = 0; i < 400 && !cancelled.load(); ++i) {
            co_await TimerAwaiter{std::chrono::milliseconds(5), std::nullopt};
        }
        if (cancelled.load() && ::send(peerFd, "x", 1, MSG_NOSIGNAL) == 1) {
            for (int i = 0; i < 400 && !received.load(); ++i) {
                co_await TimerAwaiter{std::chrono::milliseconds(5), std::nullopt};
            }
        }
        stop.requestStop();
    }
}

int main() {
    // A moved-from token is treated as stopped instead of dereferencing null state.
    {
        const CancellationTokenSource cts;
        CancellationToken token = cts.getToken();
        const CancellationToken moved = std::move(token);
        if (!token.isStopped()) return 2;
        if (moved.isStopped()) return 3;
    }

    // Cancelling one reader must not wipe another coroutine's registration on the
    // same fd: the surviving reader still has to observe data written afterwards.
    {
        int fds[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
            return 4;
        }
        const SocketPtr socket = std::make_shared<Socket>(fds[0]);
        const int peerFd = fds[1];

        std::atomic_bool cancelled{false};
        std::atomic_bool received{false};
        CancellationTokenSource readerCts;
        CancellationTokenSource stopCts;

        {
            EpollScheduler scheduler(stopCts);
            cancelledReader(socket, readerCts.getToken(), cancelled).startDetached(scheduler);
            survivingReader(socket, received).startDetached(scheduler);
            driver(peerFd, cancelled, received, stopCts).startDetached(scheduler);
            readerCts.requestStop();
            scheduler.run();
        }
        // Closing a retained socket after scheduler destruction is safe.
        socket->close();
        ::close(peerFd);
        if (!cancelled.load()) return 5;
        if (!received.load()) return 6;
    }
    return 0;
}
