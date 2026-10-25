//
// Created by vadim on 28.01.2026.
//

#ifndef CORO_LITE_SOCKET_H
#define CORO_LITE_SOCKET_H

#include "CoroTask.h"
#include "Endpoint.h"

#include <memory>
#include <mutex>
#include <span>

namespace CoroLite {
    class Socket;
    struct AcceptedSocket;

    struct ListenSocketAwaiter final : SchedulerAware<EpollScheduler> {
        ListenSocketAwaiter(int fd_, CancellationToken cancellationToken_);
        ListenSocketAwaiter(const Socket &socket_, CancellationToken cancellationToken_);

        [[nodiscard]] static bool await_ready() noexcept;

        void await_suspend(std::coroutine_handle<> h);

        [[nodiscard]] AcceptedSocket await_resume() const;

    private:
        int fd;
        const Socket *owner = nullptr;
        CancellationToken cancellationToken;
        std::coroutine_handle<> handle;
    };

    using SocketPtr = std::shared_ptr<Socket>;

    struct ReadSocketAwaiter final : SchedulerAware<EpollScheduler> {
        ReadSocketAwaiter(SocketPtr socket_, std::span<unsigned char> buffer_, CancellationTokenOpt cancellationToken_);

        bool await_ready() const noexcept;

        void await_suspend(std::coroutine_handle<> h);

        size_t await_resume();

    private:
        SocketPtr socket;
        std::span<unsigned char> buffer;
        mutable int peekErrno = 0;
        mutable bool peekEof = false;
        CancellationTokenOpt cancellationToken;
        std::coroutine_handle<> handle;
    };

    struct WriteSocketAwaiter final : SchedulerAware<EpollScheduler> {
        WriteSocketAwaiter(SocketPtr socket_,
                           std::span<unsigned char> buffer_,
                           CancellationTokenOpt cancellationToken_);

        bool await_ready() const noexcept;

        void await_suspend(std::coroutine_handle<> h);

        size_t await_resume() const;

    private:
        SocketPtr socket;
        std::span<unsigned char> buffer;
        mutable int pollErrno = 0;
        mutable bool pollError = false;
        mutable short pollRevents = 0;
        CancellationTokenOpt cancellationToken;
        std::coroutine_handle<> handle;
    };

    struct ConnectSocketAwaiter final : SchedulerAware<EpollScheduler> {
        ConnectSocketAwaiter(SocketPtr socket_, Endpoint endpoint_, CancellationTokenOpt cancellationToken_);

        bool await_ready() const noexcept;

        void await_suspend(std::coroutine_handle<> h);

        void await_resume() const;

    private:
        SocketPtr socket;
        Endpoint endpoint;
        mutable int connectErrno = 0;
        mutable bool connectPending = false;
        CancellationTokenOpt cancellationToken;
        std::coroutine_handle<> handle;
    };

    class Socket final : public std::enable_shared_from_this<Socket> {
    public:
        Socket();

        explicit Socket(int fd);

        Socket(const Socket &) = delete;

        Socket &operator=(const Socket &) = delete;

        Socket(Socket &&) = delete;

        Socket &operator=(Socket &&) = delete;

        ~Socket();

        void setReusePort(bool reusePort) const;

        void setReuseAddr(bool reuseAddr) const;

        void setScheduler(const EpollScheduler *scheduler) const noexcept;

        void close() noexcept;

        [[nodiscard]] int fd() const noexcept;

        [[nodiscard]] bool bind(const Endpoint &endpoint) const noexcept;

        [[nodiscard]] ConnectSocketAwaiter connect(Endpoint endpoint, CancellationTokenOpt ct);

        [[nodiscard]] ListenSocketAwaiter listen(CancellationToken ct) const;

        [[nodiscard]] CoroTask<size_t> read(std::span<unsigned char> buffer, CancellationTokenOpt ct);

        [[nodiscard]] CoroTask<size_t> write(std::span<unsigned char> buffer, CancellationTokenOpt ct);

        [[nodiscard]] bool isEof() const noexcept;

    private:
        UniqueFd fd_;
        mutable std::mutex registryMutex;
        mutable std::weak_ptr<FdRegistry> registry;
    };

    struct AcceptedSocket final {
        SocketPtr socket;
        Endpoint endpoint;
    };
} // namespace CoroLite

#endif // CORO_LITE_SOCKET_H
