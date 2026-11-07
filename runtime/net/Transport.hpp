#pragma once

#include "runtime/net/Socket.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace lumora::net {

/// Peer ids: the host is 0, clients 1, 2, ...
using PeerId = std::uint32_t;
inline constexpr PeerId kHostPeer = 0;

enum class Channel : std::uint8_t {
    Unreliable = 0,  // may be lost or come late (state that the next one replaces)
    Reliable = 1,    // arrives, once, in the order sent (events, spawns, calls)
};

/// The largest message one send takes (a datagram is at most ~1200 bytes: safe across the
/// internet and tunnels). Bigger data goes in several messages.
inline constexpr std::size_t kMaxMessage = 1100;

struct NetEvent {
    enum class Kind { Connected, Disconnected, Message };
    Kind kind = Kind::Message;
    PeerId peer = kHostPeer;
    Channel channel = Channel::Reliable;
    std::vector<std::byte> data;  // Message
    std::string reason;           // Disconnected
};

struct TransportStats {
    std::uint64_t packetsSent = 0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t resent = 0;          // reliable messages sent again
    std::uint64_t rejected = 0;        // datagrams that failed authentication or made no sense
    double roundTrip = 0.0;            // seconds, smoothed (clients; hosts: the first peer)
};

/// UDP connections between a host and its clients (game networking): a handshake that agrees
/// on keys (X25519, with an optional password mixed in, so a wrong password simply cannot talk),
/// every packet encrypted and authenticated (XChaCha20-Poly1305), and two channels over it:
/// unreliable, and reliable-ordered (acknowledged, resent until it arrives). Time is passed in
/// (seconds, any steady clock), so the logic runs the same under tests.
class Transport {
public:
    /// Listens on a port (0 = any) for up to `maxClients` clients.
    [[nodiscard]] static Result<std::unique_ptr<Transport>> host(std::uint16_t port, std::string password, int maxClients = 16);
    /// Starts connecting to a host; Connected (peer 0) or Disconnected follows from poll().
    [[nodiscard]] static Result<std::unique_ptr<Transport>> connect(const HostPort& address, std::string password);
    ~Transport();

    [[nodiscard]] bool isHost() const noexcept { return hosting_; }
    [[nodiscard]] std::uint16_t port() const { return socket_.localPort(); }
    /// Clients: the handshake has finished and the connection is up.
    [[nodiscard]] bool connected() const;
    /// Hosts: the connected clients; clients: the host, once connected.
    [[nodiscard]] std::vector<PeerId> peers() const;
    /// Clients: the id the host gave this client (0 until connected).
    [[nodiscard]] PeerId localId() const noexcept { return localId_; }

    /// Queues a message (at most kMaxMessage bytes) for a peer; false if there is no such
    /// connected peer or the message is too big.
    bool send(PeerId peer, Channel channel, std::span<const std::byte> data);
    /// Hosts: to every connected client (except `except`).
    void broadcast(Channel channel, std::span<const std::byte> data, PeerId except = ~PeerId{0});
    /// Closes a connection (a host drops a client; a client leaves), telling the other side.
    void disconnect(PeerId peer, const std::string& reason = "left");

    /// Receives what arrived, resends what was not acknowledged, sends what is queued and
    /// keep-alives, and notices timeouts. Returns what happened, in order.
    [[nodiscard]] std::vector<NetEvent> poll(double now);

    [[nodiscard]] const TransportStats& stats() const noexcept { return stats_; }
    /// Tests: drop this share of outgoing datagrams (0..1).
    void simulatePacketLoss(float share, std::uint32_t seed = 1);

    struct Connection;  // (internal; public for the implementation's helpers)

private:
    Transport() = default;
    void receiveDatagram(std::span<const std::byte> datagram, const Endpoint& from, double now, std::vector<NetEvent>& events);
    void flush(Connection& c, double now);
    void sendRaw(const Endpoint& to, std::span<const std::byte> datagram);
    [[nodiscard]] Connection* find(PeerId peer);
    void drop(Connection& c, const std::string& reason, std::vector<NetEvent>& events, bool tell);

    bool hosting_ = false;
    UdpSocket socket_;
    std::string password_;
    int maxClients_ = 16;
    PeerId nextPeer_ = 1;
    PeerId localId_ = 0;
    std::vector<std::unique_ptr<Connection>> connections_;
    TransportStats stats_;
    std::mt19937 lossRandom_{1};
    float loss_ = 0.0f;
    std::vector<NetEvent> pendingEvents_;  // raised outside poll (disconnect())
};

} // namespace lumora::net
