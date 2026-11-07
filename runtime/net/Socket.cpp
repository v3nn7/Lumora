#include "runtime/net/Socket.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLength = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLength = socklen_t;
#endif

namespace lumora::net {

namespace {

#if defined(_WIN32)
void startup() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
void closeHandle(std::intptr_t h) { closesocket(static_cast<SOCKET>(h)); }
bool wouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAECONNRESET;  // (an ICMP "port unreachable" on UDP)
}
int lastError() { return WSAGetLastError(); }
#else
void startup() {}
void closeHandle(std::intptr_t h) { ::close(static_cast<int>(h)); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED; }
int lastError() { return errno; }
#endif

bool setNonBlocking(std::intptr_t h) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(static_cast<SOCKET>(h), FIONBIO, &on) == 0;
#else
    const int flags = fcntl(static_cast<int>(h), F_GETFL, 0);
    return flags >= 0 && fcntl(static_cast<int>(h), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

const sockaddr* address(const Endpoint& e) { return reinterpret_cast<const sockaddr*>(e.storage.data()); }

/// IPv4 endpoints as IPv4-mapped IPv6 for a dual-stack socket, and back.
Endpoint toMapped(const Endpoint& e) {
    if (e.ipv6()) return e;
    sockaddr_in in4{};
    std::memcpy(&in4, e.storage.data(), sizeof in4);
    sockaddr_in6 in6{};
    in6.sin6_family = AF_INET6;
    in6.sin6_port = in4.sin_port;
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&in6.sin6_addr);
    bytes[10] = bytes[11] = 0xFF;
    std::memcpy(bytes + 12, &in4.sin_addr, 4);
    Endpoint out;
    std::memcpy(out.storage.data(), &in6, sizeof in6);
    out.size = sizeof in6;
    return out;
}

Endpoint fromMapped(const Endpoint& e) {
    if (!e.ipv6()) return e;
    sockaddr_in6 in6{};
    std::memcpy(&in6, e.storage.data(), sizeof in6);
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&in6.sin6_addr);
    static constexpr unsigned char kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(bytes, kPrefix, 12) != 0) return e;
    sockaddr_in in4{};
    in4.sin_family = AF_INET;
    in4.sin_port = in6.sin6_port;
    std::memcpy(&in4.sin_addr, bytes + 12, 4);
    Endpoint out;
    std::memcpy(out.storage.data(), &in4, sizeof in4);
    out.size = sizeof in4;
    return out;
}

} // namespace

std::optional<HostPort> parseHostPort(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    for (std::string_view scheme : {"lumora://", "udp://"}) {
        if (text.size() >= scheme.size() &&
            std::equal(scheme.begin(), scheme.end(), text.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
            text.remove_prefix(scheme.size());
        }
    }
    while (!text.empty() && text.back() == '/') text.remove_suffix(1);
    if (text.empty() || text.find(' ') != std::string_view::npos) return std::nullopt;
    HostPort out;
    std::string_view port;
    if (text.front() == '[') {  // [ipv6]:port
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        out.host = std::string(text.substr(1, close - 1));
        const std::string_view rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return std::nullopt;
            port = rest.substr(1);
            if (port.empty()) return std::nullopt;
        }
    } else if (std::ranges::count(text, ':') == 1) {
        const std::size_t colon = text.find(':');
        out.host = std::string(text.substr(0, colon));
        port = text.substr(colon + 1);
        if (port.empty()) return std::nullopt;
    } else {
        out.host = std::string(text);  // a name, or a bare IPv6 address
    }
    if (out.host.empty()) return std::nullopt;
    if (!port.empty()) {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (ec != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535) return std::nullopt;
        out.port = static_cast<std::uint16_t>(value);
    }
    return out;
}

std::string formatHostPort(const HostPort& a) {
    return a.host.find(':') != std::string::npos ? std::format("[{}]:{}", a.host, a.port) : std::format("{}:{}", a.host, a.port);
}

bool Endpoint::operator==(const Endpoint& other) const noexcept {
    const Endpoint a = fromMapped(*this), b = fromMapped(other);
    return a.size == b.size && std::memcmp(a.storage.data(), b.storage.data(), a.size) == 0;
}

bool Endpoint::ipv6() const noexcept { return size != 0 && address(*this)->sa_family == AF_INET6; }

std::uint16_t Endpoint::port() const noexcept {
    if (size == 0) return 0;
    if (ipv6()) {
        sockaddr_in6 in6{};
        std::memcpy(&in6, storage.data(), sizeof in6);
        return ntohs(in6.sin6_port);
    }
    sockaddr_in in4{};
    std::memcpy(&in4, storage.data(), sizeof in4);
    return ntohs(in4.sin_port);
}

std::string Endpoint::toString() const {
    const Endpoint e = fromMapped(*this);
    if (e.size == 0) return "(none)";
    char text[INET6_ADDRSTRLEN] = {};
    if (e.ipv6()) {
        sockaddr_in6 in6{};
        std::memcpy(&in6, e.storage.data(), sizeof in6);
        inet_ntop(AF_INET6, &in6.sin6_addr, text, sizeof text);
        return std::format("[{}]:{}", text, e.port());
    }
    sockaddr_in in4{};
    std::memcpy(&in4, e.storage.data(), sizeof in4);
    inet_ntop(AF_INET, &in4.sin_addr, text, sizeof text);
    return std::format("{}:{}", text, e.port());
}

Result<Endpoint> resolve(const HostPort& a) {
    startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* list = nullptr;
    const std::string port = std::to_string(a.port);
    if (const int r = getaddrinfo(a.host.c_str(), port.c_str(), &hints, &list); r != 0 || list == nullptr) {
        return makeError("cannot find '{}'", a.host);
    }
    Endpoint best;
    for (int pass = 0; pass < 2 && !best.valid(); ++pass) {  // IPv4 first: tunnels and LANs mostly speak it
        for (addrinfo* it = list; it != nullptr; it = it->ai_next) {
            if ((pass == 0) != (it->ai_family == AF_INET) || it->ai_addrlen > best.storage.size()) continue;
            std::memcpy(best.storage.data(), it->ai_addr, it->ai_addrlen);
            best.size = static_cast<std::uint32_t>(it->ai_addrlen);
            break;
        }
    }
    freeaddrinfo(list);
    if (!best.valid()) return makeError("no usable address for '{}'", a.host);
    return best;
}

UdpSocket::~UdpSocket() {
    if (handle_ >= 0) closeHandle(handle_);
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_), dualStack_(other.dualStack_) { other.handle_ = -1; }

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        if (handle_ >= 0) closeHandle(handle_);
        handle_ = other.handle_;
        dualStack_ = other.dualStack_;
        other.handle_ = -1;
    }
    return *this;
}

Result<UdpSocket> UdpSocket::bind(std::uint16_t port) {
    startup();
    // Dual stack (IPv6 socket that also takes IPv4) where the system allows it.
    auto h = static_cast<std::intptr_t>(::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP));
    if (h >= 0) {
        int off = 0;
        setsockopt(static_cast<decltype(::socket(0, 0, 0))>(h), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
        sockaddr_in6 any{};
        any.sin6_family = AF_INET6;
        any.sin6_addr = in6addr_any;
        any.sin6_port = htons(port);
        if (::bind(static_cast<decltype(::socket(0, 0, 0))>(h), reinterpret_cast<sockaddr*>(&any), sizeof any) == 0 && setNonBlocking(h)) {
            return UdpSocket(h, true);
        }
        closeHandle(h);
    }
    h = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (h < 0) return makeError("cannot create a UDP socket (error {})", lastError());
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = htons(port);
    if (::bind(static_cast<decltype(::socket(0, 0, 0))>(h), reinterpret_cast<sockaddr*>(&any), sizeof any) != 0) {
        const int error = lastError();
        closeHandle(h);
        return makeError("cannot use UDP port {} (error {}; is it taken?)", port, error);
    }
    if (!setNonBlocking(h)) {
        closeHandle(h);
        return makeError("cannot make the UDP socket non-blocking");
    }
    return UdpSocket(h, false);
}

Result<UdpSocket> UdpSocket::open(const Endpoint& forPeer) {
    startup();
    const int family = forPeer.ipv6() ? AF_INET6 : AF_INET;
    const auto h = static_cast<std::intptr_t>(::socket(family, SOCK_DGRAM, IPPROTO_UDP));
    if (h < 0) return makeError("cannot create a UDP socket (error {})", lastError());
    if (!setNonBlocking(h)) {
        closeHandle(h);
        return makeError("cannot make the UDP socket non-blocking");
    }
    return UdpSocket(h, false);
}

std::uint16_t UdpSocket::localPort() const {
    if (handle_ < 0) return 0;
    Endpoint e;
    SocketLength length = static_cast<SocketLength>(e.storage.size());
    if (getsockname(static_cast<decltype(::socket(0, 0, 0))>(handle_), reinterpret_cast<sockaddr*>(e.storage.data()), &length) != 0) return 0;
    e.size = static_cast<std::uint32_t>(length);
    return e.port();
}

bool UdpSocket::send(const Endpoint& to, std::span<const std::byte> data) {
    if (handle_ < 0 || !to.valid()) return false;
    const Endpoint target = dualStack_ ? toMapped(to) : to;
    const auto sent = ::sendto(static_cast<decltype(::socket(0, 0, 0))>(handle_), reinterpret_cast<const char*>(data.data()),
                               static_cast<int>(data.size()), 0, address(target), static_cast<SocketLength>(target.size));
    if (sent < 0 && !wouldBlock()) {
        static int reported = 0;
        if (reported != lastError()) log::warn("Net", "send to {} failed (error {})", to.toString(), lastError());
        reported = lastError();
        return false;
    }
    return sent >= 0;
}

std::optional<std::size_t> UdpSocket::receive(std::span<std::byte> buffer, Endpoint& from) {
    if (handle_ < 0) return std::nullopt;
    for (;;) {
        Endpoint source;
        SocketLength length = static_cast<SocketLength>(source.storage.size());
        const auto got = ::recvfrom(static_cast<decltype(::socket(0, 0, 0))>(handle_), reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(source.storage.data()), &length);
        if (got < 0) {
            if (wouldBlock()) {
#if defined(_WIN32)
                if (WSAGetLastError() == WSAECONNRESET) continue;  // a stale ICMP error: the next datagram
#endif
                return std::nullopt;
            }
            return std::nullopt;
        }
        source.size = static_cast<std::uint32_t>(length);
        from = fromMapped(source);
        return static_cast<std::size_t>(got);
    }
}

} // namespace lumora::net
