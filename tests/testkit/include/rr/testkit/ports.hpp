// rr/testkit/ports.hpp — TCP ports for tests that start real servers.
//
// Why this exists
//   Some tests start processes that must be told their port in advance (the
//   runtime's engines listen on consecutive ports from a configured one).
//   Ports are drawn at random from 20000–32000:
//     * below Linux's ephemeral range (32768–60999), where the kernel picks
//       the local ports of outgoing connections, so none of the test's own
//       client sockets can take a chosen port between the check and the
//       server's bind();
//     * at random, so test processes running in parallel stay out of each
//       other's way.
//   A server that can bind port 0 and report the result should do that
//   instead; this is for the ones that cannot.
//
// Thread-safety: plain functions.
#pragma once

#include <cstdint>
#include <random>
#include <stdexcept>

#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace rr::testkit {

namespace detail {

inline sockaddr_in loopback(int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

}  // namespace detail

// Whether 127.0.0.1:`port` can be bound right now.
inline bool can_bind(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    const sockaddr_in address = detail::loopback(port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    const bool bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return bound;
}

// Whether something accepts connections on 127.0.0.1:`port`.
inline bool can_connect(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    const sockaddr_in address = detail::loopback(port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    const bool connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return connected;
}

// The first of `count` consecutive ports that are free now (see above).
inline int free_port_range(int count) {
    std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<int> base_of{20'000, 32'000 - count};
    for (int attempt = 0; attempt < 200; ++attempt) {
        const int base = base_of(random);
        bool all_free = true;
        for (int offset = 0; offset < count && all_free; ++offset) {
            all_free = can_bind(base + offset);
        }
        if (all_free) {
            return base;
        }
    }
    throw std::runtime_error("no range of free ports");
}

}  // namespace rr::testkit
