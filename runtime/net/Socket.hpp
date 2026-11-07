#pragma once

#include "core/Error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lumora::net {

inline constexpr std::uint16_t kDefaultPort = 7778;  // (the editor's collaboration uses 7777)

/// "host", "host:port", "[ipv6]:port" or "lumora://host:port": a LAN address
/// ("192.168.1.20:7778") or a tunnel's ("name.at.ply.gg:12345", playit.gg).
struct HostPort {
    std::string host;
    std::uint16_t port = kDefaultPort;
};
[[nodiscard]] std::optional<HostPort> parseHostPort(std::string_view text);
[[nodiscard]] std::string formatHostPort(const HostPort& address);

/// A resolved socket address (IPv4 or IPv6), comparable and printable.
struct Endpoint {
    std::array<std::byte, 28> storage{};  // sockaddr_in / sockaddr_in6
    std::uint32_t size = 0;
    [[nodiscard]] bool valid() const noexcept { return size != 0; }
    [[nodiscard]] bool operator==(const Endpoint& other) const noexcept;
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool ipv6() const noexcept;
};

/// Looks a host up (DNS for names); IPv4 first.
[[nodiscard]] Result<Endpoint> resolve(const HostPort& address);

/// A non-blocking UDP socket.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Opens a socket on a port (0 = any free one) for every address (dual stack where it can,
    /// IPv4 otherwise), or one for the family of `forPeer` (clients).
    [[nodiscard]] static Result<UdpSocket> bind(std::uint16_t port);
    [[nodiscard]] static Result<UdpSocket> open(const Endpoint& forPeer);

    [[nodiscard]] bool isOpen() const noexcept { return handle_ >= 0; }
    [[nodiscard]] std::uint16_t localPort() const;
    /// False if the datagram could not be handed to the system (logged once per error kind).
    bool send(const Endpoint& to, std::span<const std::byte> data);
    /// The next datagram waiting (its length), or nothing.
    [[nodiscard]] std::optional<std::size_t> receive(std::span<std::byte> buffer, Endpoint& from);

private:
    explicit UdpSocket(std::intptr_t handle, bool dualStack) : handle_(handle), dualStack_(dualStack) {}
    std::intptr_t handle_ = -1;
    bool dualStack_ = false;
};

} // namespace lumora::net
