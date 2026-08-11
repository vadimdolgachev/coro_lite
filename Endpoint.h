//
// Created by vadim on 29.01.2026.
//

#ifndef CORO_LITE_ENDPOINT_H
#define CORO_LITE_ENDPOINT_H

#include <cstddef>
#include <cstring>
#include <netinet/in.h>
#include <functional>
#include <string>
#include <variant>

namespace CoroLite {
    class Endpoint final {
    public:
        enum class Type {
            IPv4,
            IPv6,
            Hostname
        };

        Endpoint() = default;

        explicit Endpoint(const sockaddr_in &addr_);

        explicit Endpoint(const sockaddr_in6 &addr6_);

        explicit Endpoint(uint16_t port);

        Endpoint(const std::string &host, uint16_t port);

        Endpoint(const sockaddr_storage &storage_, socklen_t len);

        [[nodiscard]] Type type() const noexcept;

        [[nodiscard]] uint16_t port() const;

        [[nodiscard]] uint32_t ip() const;

        [[nodiscard]] std::string ipStr() const;

        [[nodiscard]] std::string toString() const;

        [[nodiscard]] std::string host() const;

        [[nodiscard]] bool isIP() const noexcept;

        [[nodiscard]] bool isIPv4() const noexcept;

        [[nodiscard]] bool isIPv6() const noexcept;

        [[nodiscard]] bool isHostname() const noexcept;

        [[nodiscard]] const sockaddr_in &sockaddr() const;

        [[nodiscard]] const sockaddr_in6 &sockaddr6() const;

        [[nodiscard]] std::pair<sockaddr_storage, socklen_t> sockaddrStorage() const;

    private:
        using IPv4Addr = sockaddr_in;

        using IPv6Addr = sockaddr_in6;

        struct HostnameAddr {
            std::string host;
            uint16_t port;
        };

        std::variant<IPv4Addr, IPv6Addr, HostnameAddr> storage{IPv4Addr{}};

        friend bool operator==(const Endpoint &lhs, const Endpoint &rhs) noexcept;

        friend struct std::hash<Endpoint>;
    };

    bool operator==(const Endpoint &lhs, const Endpoint &rhs) noexcept;
} // namespace CoroLite

template<>
struct std::hash<CoroLite::Endpoint> {
    size_t operator()(const CoroLite::Endpoint &endpoint) const noexcept;
};

#endif //CORO_LITE_ENDPOINT_H
