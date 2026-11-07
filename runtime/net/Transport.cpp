#include "runtime/net/Transport.hpp"

#include "core/Log.hpp"

#include <monocypher.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace lumora::net {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'L', 'M', 'N', 'T'};
constexpr std::uint8_t kVersion = 1;
enum class PacketType : std::uint8_t { Hello = 1, Welcome = 2, Reject = 3, Data = 4 };
enum class RejectReason : std::uint8_t { Full = 1, Version = 2 };

constexpr std::size_t kHeader = 6;           // magic, version, type
constexpr std::size_t kHelloSize = 128;      // padded: an answer is never larger than the question
constexpr std::size_t kMaxDatagram = 1280;
constexpr std::size_t kSeal = 4 + 24 + 16;   // connection id, nonce, MAC
constexpr std::size_t kPayloadHeader = 2 + 2 + 4 + 1;  // sequence, ack, ack bits, flags
constexpr std::size_t kMessageHeader = 1 + 2 + 2;      // channel, reliable id, length
constexpr double kHelloInterval = 0.25;
constexpr double kConnectTimeout = 10.0;
constexpr double kTimeout = 10.0;
constexpr double kKeepAlive = 0.1;
constexpr int kMaxPacketsPerFlush = 64;
constexpr std::uint8_t kFlagDisconnect = 1;
constexpr std::uint8_t kFlagAck = 2;  // the ack fields mean something (a packet of the other side arrived)
constexpr std::array<std::uint8_t, 9> kWelcomeProof{'L', 'U', 'M', 'O', 'R', 'A', '-', 'O', 'K'};

using Key = std::array<std::uint8_t, 32>;

void randomBytes(std::span<std::uint8_t> out) {
    static thread_local std::random_device device;
    for (std::size_t i = 0; i < out.size(); i += 4) {
        const std::uint32_t r = device();
        for (std::size_t k = 0; k < 4 && i + k < out.size(); ++k) out[i + k] = static_cast<std::uint8_t>(r >> (8 * k));
    }
}

bool newer(std::uint16_t a, std::uint16_t b) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0; }

struct Writer {
    std::vector<std::byte>& out;
    void u8(std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void bytes(std::span<const std::byte> b) { out.insert(out.end(), b.begin(), b.end()); }
    void bytes(std::span<const std::uint8_t> b) { bytes(std::as_bytes(b)); }
};

struct Reader {
    std::span<const std::byte> data;
    std::size_t at = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (at + n > data.size()) ok = false;
        return ok;
    }
    std::uint8_t u8() { return need(1) ? static_cast<std::uint8_t>(data[at++]) : 0; }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
    std::uint32_t u32() {
        const std::uint32_t lo = u16();
        return lo | (static_cast<std::uint32_t>(u16()) << 16);
    }
    std::span<const std::byte> bytes(std::size_t n) {
        if (!need(n)) return {};
        const auto s = data.subspan(at, n);
        at += n;
        return s;
    }
};

void writeHeader(Writer& w, PacketType type) {
    w.bytes(std::span<const std::uint8_t>(kMagic));
    w.u8(kVersion);
    w.u8(static_cast<std::uint8_t>(type));
}

/// Keys of a connection: X25519 between the two ephemeral keys, both public keys and the
/// password hashed together; client-to-host and host-to-client halves.
void deriveKeys(const Key& mySecret, const Key& theirPublic, const Key& clientPublic, const Key& hostPublic,
                const std::string& password, Key& clientToHost, Key& hostToClient) {
    std::array<std::uint8_t, 128> material{};
    crypto_x25519(material.data(), mySecret.data(), theirPublic.data());
    std::memcpy(material.data() + 32, clientPublic.data(), 32);
    std::memcpy(material.data() + 64, hostPublic.data(), 32);
    crypto_blake2b(material.data() + 96, 32, reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    std::array<std::uint8_t, 64> out{};
    crypto_blake2b(out.data(), out.size(), material.data(), material.size());
    std::memcpy(clientToHost.data(), out.data(), 32);
    std::memcpy(hostToClient.data(), out.data() + 32, 32);
    crypto_wipe(material.data(), material.size());
    crypto_wipe(out.data(), out.size());
}

} // namespace

struct Transport::Connection {
    PeerId id = 0;
    Endpoint endpoint;
    enum class State { Handshake, Connected } state = State::Handshake;
    Key secret{}, publicKey{}, peerPublic{};
    std::array<std::uint8_t, 16> salt{};  // the client's, echoed in the Welcome
    Key sendKey{}, receiveKey{};
    std::vector<std::byte> welcome;  // hosts: the Welcome, resent when a Hello repeats
    double created = 0.0, lastReceived = 0.0, lastSent = -1.0, lastHello = -1.0;
    // Packets
    std::uint16_t nextSequence = 0;
    std::uint16_t remoteSequence = 0;
    std::uint32_t receivedBits = 0;  // bit i: packet remoteSequence - 1 - i arrived
    bool anyReceived = false;
    bool ackPending = false;
    std::map<std::uint16_t, double> sentTimes;
    std::map<std::uint16_t, std::vector<std::uint16_t>> packetMessages;  // reliable ids each packet carried
    double roundTrip = 0.1;
    // Reliable channel
    struct Outgoing {
        std::uint16_t id = 0;
        std::vector<std::byte> data;
        double lastSent = -1.0;
    };
    std::deque<Outgoing> reliableOut;
    std::uint16_t nextReliable = 0;
    std::uint16_t expectedReliable = 0;
    std::map<std::uint16_t, std::vector<std::byte>> reliableIn;  // arrived early
    // Unreliable channel
    std::deque<std::vector<std::byte>> unreliableOut;

    [[nodiscard]] bool hasWork() const { return !reliableOut.empty() || !unreliableOut.empty(); }
};

Transport::~Transport() {
    for (auto& c : connections_) {
        if (c->state == Connection::State::Connected) {
            std::vector<NetEvent> ignored;
            drop(*c, "closed", ignored, true);
        }
    }
}

Result<std::unique_ptr<Transport>> Transport::host(std::uint16_t port, std::string password, int maxClients) {
    auto socket = UdpSocket::bind(port);
    if (!socket) return std::unexpected(socket.error());
    std::unique_ptr<Transport> t(new Transport());
    t->hosting_ = true;
    t->socket_ = std::move(*socket);
    t->password_ = std::move(password);
    t->maxClients_ = std::max(maxClients, 1);
    return t;
}

Result<std::unique_ptr<Transport>> Transport::connect(const HostPort& address, std::string password) {
    auto endpoint = resolve(address);
    if (!endpoint) return std::unexpected(endpoint.error());
    auto socket = UdpSocket::open(*endpoint);
    if (!socket) return std::unexpected(socket.error());
    std::unique_ptr<Transport> t(new Transport());
    t->socket_ = std::move(*socket);
    t->password_ = std::move(password);
    auto c = std::make_unique<Connection>();
    c->id = kHostPeer;
    c->endpoint = *endpoint;
    randomBytes(c->secret);
    crypto_x25519_public_key(c->publicKey.data(), c->secret.data());
    randomBytes(c->salt);
    c->created = -1.0;  // set on the first poll
    t->connections_.push_back(std::move(c));
    return t;
}

bool Transport::connected() const {
    return !hosting_ && !connections_.empty() && connections_.front()->state == Connection::State::Connected;
}

std::vector<PeerId> Transport::peers() const {
    std::vector<PeerId> out;
    for (const auto& c : connections_) {
        if (c->state == Connection::State::Connected) out.push_back(c->id);
    }
    return out;
}

Transport::Connection* Transport::find(PeerId peer) {
    for (auto& c : connections_) {
        if (c->id == peer && c->state == Connection::State::Connected) return c.get();
    }
    return nullptr;
}

bool Transport::send(PeerId peer, Channel channel, std::span<const std::byte> data) {
    if (data.size() > kMaxMessage) {
        log::warn("Net", "message of {} bytes is over the limit of {}", data.size(), kMaxMessage);
        return false;
    }
    Connection* c = find(peer);
    if (c == nullptr) return false;
    if (channel == Channel::Reliable) {
        c->reliableOut.push_back({c->nextReliable++, std::vector<std::byte>(data.begin(), data.end()), -1.0});
    } else {
        c->unreliableOut.emplace_back(data.begin(), data.end());
    }
    return true;
}

void Transport::broadcast(Channel channel, std::span<const std::byte> data, PeerId except) {
    for (const auto& c : connections_) {
        if (c->state == Connection::State::Connected && c->id != except) send(c->id, channel, data);
    }
}

void Transport::disconnect(PeerId peer, const std::string& reason) {
    for (auto& c : connections_) {
        if (c->id == peer) {
            drop(*c, reason, pendingEvents_, true);
            break;
        }
    }
    std::erase_if(connections_, [&](const auto& c) { return c->id == peer; });
}

void Transport::simulatePacketLoss(float share, std::uint32_t seed) {
    loss_ = std::clamp(share, 0.0f, 1.0f);
    lossRandom_.seed(seed);
}

void Transport::sendRaw(const Endpoint& to, std::span<const std::byte> datagram) {
    ++stats_.packetsSent;
    if (loss_ > 0.0f && std::uniform_real_distribution<float>(0.0f, 1.0f)(lossRandom_) < loss_) return;
    socket_.send(to, datagram);
}

void Transport::drop(Connection& c, const std::string& reason, std::vector<NetEvent>& events, bool tell) {
    if (tell && c.state == Connection::State::Connected) {
        // A few unreliable goodbyes: the other side stops waiting at once instead of timing out.
        for (int i = 0; i < 3; ++i) {
            std::vector<std::byte> plain;
            Writer p{plain};
            p.u16(c.nextSequence++);
            p.u16(c.remoteSequence);
            p.u32(c.receivedBits);
            p.u8(static_cast<std::uint8_t>(kFlagDisconnect | (c.anyReceived ? kFlagAck : 0)));
            std::vector<std::byte> datagram;
            Writer w{datagram};
            writeHeader(w, PacketType::Data);
            w.u32(hosting_ ? c.id : localId_);
            std::array<std::uint8_t, 24> nonce{};
            randomBytes(nonce);
            std::array<std::uint8_t, 16> mac{};
            std::vector<std::byte> cipher(plain.size());
            crypto_aead_lock(reinterpret_cast<std::uint8_t*>(cipher.data()), mac.data(), c.sendKey.data(), nonce.data(),
                             reinterpret_cast<const std::uint8_t*>(datagram.data()), datagram.size(),
                             reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size());
            w.bytes(std::span<const std::uint8_t>(nonce));
            w.bytes(std::span<const std::uint8_t>(mac));
            w.bytes(std::span<const std::byte>(cipher));
            sendRaw(c.endpoint, datagram);
        }
    }
    if (c.state == Connection::State::Connected || !hosting_) {
        events.push_back({NetEvent::Kind::Disconnected, c.id, Channel::Reliable, {}, reason});
    }
    c.state = Connection::State::Handshake;
    crypto_wipe(c.secret.data(), c.secret.size());
    crypto_wipe(c.sendKey.data(), c.sendKey.size());
    crypto_wipe(c.receiveKey.data(), c.receiveKey.size());
}

std::vector<NetEvent> Transport::poll(double now) {
    std::vector<NetEvent> events = std::move(pendingEvents_);
    pendingEvents_.clear();
    // Receive everything waiting.
    std::array<std::byte, 2048> buffer{};
    for (int i = 0; i < 4096; ++i) {
        Endpoint from;
        const auto size = socket_.receive(buffer, from);
        if (!size) break;
        ++stats_.packetsReceived;
        receiveDatagram(std::span(buffer.data(), *size), from, now, events);
    }
    // Handshakes, timeouts and sending.
    for (auto it = connections_.begin(); it != connections_.end();) {
        Connection& c = **it;
        bool remove = false;
        if (!hosting_ && c.state == Connection::State::Handshake) {
            if (c.created < 0.0) c.created = now;
            if (now - c.created > kConnectTimeout) {
                events.push_back({NetEvent::Kind::Disconnected, kHostPeer, Channel::Reliable, {}, "the host did not answer"});
                remove = true;
            } else if (c.lastHello < 0.0 || now - c.lastHello >= kHelloInterval) {
                std::vector<std::byte> hello;
                Writer w{hello};
                writeHeader(w, PacketType::Hello);
                w.bytes(std::span<const std::uint8_t>(c.publicKey));
                w.bytes(std::span<const std::uint8_t>(c.salt));
                hello.resize(kHelloSize, std::byte{0});
                sendRaw(c.endpoint, hello);
                c.lastHello = now;
            }
        } else if (hosting_ && c.state == Connection::State::Handshake) {
            remove = now - c.created > kConnectTimeout;  // never confirmed (e.g. a wrong password)
        } else if (now - c.lastReceived > kTimeout) {
            drop(c, "timed out", events, false);
            remove = true;
        } else {
            flush(c, now);
        }
        it = remove ? connections_.erase(it) : it + 1;
    }
    if (!hosting_ && !connections_.empty()) stats_.roundTrip = connections_.front()->roundTrip;
    if (hosting_) {
        for (const auto& c : connections_) {
            if (c->state == Connection::State::Connected) {
                stats_.roundTrip = c->roundTrip;
                break;
            }
        }
    }
    return events;
}

void Transport::receiveDatagram(std::span<const std::byte> datagram, const Endpoint& from, double now, std::vector<NetEvent>& events) {
    Reader r{datagram};
    const auto magic = r.bytes(4);
    if (!r.ok || std::memcmp(magic.data(), kMagic.data(), 4) != 0) {
        ++stats_.rejected;
        return;
    }
    const std::uint8_t version = r.u8();
    const auto type = static_cast<PacketType>(r.u8());
    if (!r.ok) {
        ++stats_.rejected;
        return;
    }
    if (version != kVersion && type != PacketType::Reject) {
        if (hosting_ && type == PacketType::Hello) {
            std::vector<std::byte> reject;
            Writer w{reject};
            writeHeader(w, PacketType::Reject);
            w.u8(static_cast<std::uint8_t>(RejectReason::Version));
            sendRaw(from, reject);
        }
        ++stats_.rejected;
        return;
    }
    switch (type) {
        case PacketType::Hello: {
            if (!hosting_ || datagram.size() < kHelloSize) break;
            Key clientPublic{};
            std::memcpy(clientPublic.data(), r.bytes(32).data(), 32);
            std::array<std::uint8_t, 16> salt{};
            std::memcpy(salt.data(), r.bytes(16).data(), 16);
            // A repeated Hello (the Welcome got lost): the same answer.
            for (auto& c : connections_) {
                if (c->endpoint == from) {
                    if (c->peerPublic == clientPublic && c->salt == salt) {
                        sendRaw(from, c->welcome);
                        return;
                    }
                    // A new attempt from the same place replaces the old connection.
                    if (c->state == Connection::State::Connected) drop(*c, "reconnected", events, false);
                    c->endpoint = Endpoint{};
                }
            }
            std::erase_if(connections_, [](const auto& c) { return !c->endpoint.valid(); });
            if (static_cast<int>(connections_.size()) >= maxClients_) {
                std::vector<std::byte> reject;
                Writer w{reject};
                writeHeader(w, PacketType::Reject);
                w.u8(static_cast<std::uint8_t>(RejectReason::Full));
                sendRaw(from, reject);
                return;
            }
            auto c = std::make_unique<Connection>();
            c->id = nextPeer_++;
            c->endpoint = from;
            c->peerPublic = clientPublic;
            c->salt = salt;
            c->created = c->lastReceived = now;
            randomBytes(c->secret);
            crypto_x25519_public_key(c->publicKey.data(), c->secret.data());
            deriveKeys(c->secret, clientPublic, clientPublic, c->publicKey, password_, c->receiveKey, c->sendKey);
            // Welcome: the id and the host's key, with a proof sealed under the new keys (a
            // client with another password cannot open it).
            Writer w{c->welcome};
            writeHeader(w, PacketType::Welcome);
            w.u32(c->id);
            w.bytes(std::span<const std::uint8_t>(c->publicKey));
            const std::size_t adSize = c->welcome.size();
            std::array<std::uint8_t, 25> proof{};
            std::memcpy(proof.data(), salt.data(), 16);
            std::memcpy(proof.data() + 16, kWelcomeProof.data(), kWelcomeProof.size());
            std::array<std::uint8_t, 24> nonce{};
            randomBytes(nonce);
            std::array<std::uint8_t, 16> mac{};
            std::array<std::uint8_t, 25> sealed{};
            crypto_aead_lock(sealed.data(), mac.data(), c->sendKey.data(), nonce.data(), reinterpret_cast<const std::uint8_t*>(c->welcome.data()),
                             adSize, proof.data(), proof.size());
            w.bytes(std::span<const std::uint8_t>(nonce));
            w.bytes(std::span<const std::uint8_t>(mac));
            w.bytes(std::span<const std::uint8_t>(sealed));
            sendRaw(from, c->welcome);
            connections_.push_back(std::move(c));
            return;
        }
        case PacketType::Welcome: {
            if (hosting_ || connections_.empty()) break;
            Connection& c = *connections_.front();
            if (c.state != Connection::State::Handshake) break;  // (authenticated below: the address may differ)
            const std::size_t adSize = kHeader + 4 + 32;
            const std::uint32_t id = r.u32();
            Key hostPublic{};
            const auto key = r.bytes(32);
            const auto nonce = r.bytes(24);
            const auto mac = r.bytes(16);
            const auto sealed = r.bytes(25);
            if (!r.ok) break;
            std::memcpy(hostPublic.data(), key.data(), 32);
            deriveKeys(c.secret, hostPublic, c.publicKey, hostPublic, password_, c.sendKey, c.receiveKey);
            std::array<std::uint8_t, 25> proof{};
            const bool opened = crypto_aead_unlock(proof.data(), reinterpret_cast<const std::uint8_t*>(mac.data()), c.receiveKey.data(),
                                                   reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                                   reinterpret_cast<const std::uint8_t*>(datagram.data()), adSize,
                                                   reinterpret_cast<const std::uint8_t*>(sealed.data()), sealed.size()) == 0;
            if (!opened || std::memcmp(proof.data(), c.salt.data(), 16) != 0 ||
                std::memcmp(proof.data() + 16, kWelcomeProof.data(), kWelcomeProof.size()) != 0) {
                ++stats_.rejected;
                events.push_back({NetEvent::Kind::Disconnected, kHostPeer, Channel::Reliable, {}, "wrong password"});
                connections_.clear();
                return;
            }
            c.peerPublic = hostPublic;
            c.state = Connection::State::Connected;
            c.lastReceived = now;
            c.ackPending = true;  // tell the host at once
            localId_ = id;
            events.push_back({NetEvent::Kind::Connected, kHostPeer, Channel::Reliable, {}, {}});
            return;
        }
        case PacketType::Reject: {
            if (hosting_ || connections_.empty() || !(from == connections_.front()->endpoint) ||
                connections_.front()->state != Connection::State::Handshake) {
                break;
            }
            const auto reason = static_cast<RejectReason>(r.u8());
            events.push_back({NetEvent::Kind::Disconnected, kHostPeer, Channel::Reliable, {},
                              reason == RejectReason::Full ? "the game is full" : "the host runs another version of the engine"});
            connections_.clear();
            return;
        }
        case PacketType::Data: {
            const std::uint32_t id = r.u32();
            const auto nonce = r.bytes(24);
            const auto mac = r.bytes(16);
            if (!r.ok) break;
            const auto cipher = datagram.subspan(r.at);
            Connection* c = nullptr;
            for (auto& candidate : connections_) {
                if (hosting_ ? candidate->id == id : (candidate->id == kHostPeer && id == localId_)) c = candidate.get();
            }
            if (c == nullptr) break;
            std::vector<std::byte> plain(cipher.size());
            if (crypto_aead_unlock(reinterpret_cast<std::uint8_t*>(plain.data()), reinterpret_cast<const std::uint8_t*>(mac.data()),
                                   c->receiveKey.data(), reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                   reinterpret_cast<const std::uint8_t*>(datagram.data()), kHeader + 4,
                                   reinterpret_cast<const std::uint8_t*>(cipher.data()), cipher.size()) != 0) {
                break;  // forged, altered, or another password
            }
            c->endpoint = from;  // (authenticated: follows a NAT that moved the port)
            c->lastReceived = now;
            if (c->state == Connection::State::Handshake) {
                c->state = Connection::State::Connected;  // hosts: the client has the keys
                events.push_back({NetEvent::Kind::Connected, c->id, Channel::Reliable, {}, {}});
            }
            Reader p{plain};
            const std::uint16_t sequence = p.u16();
            const std::uint16_t ack = p.u16();
            const std::uint32_t ackBits = p.u32();
            const std::uint8_t flags = p.u8();
            if (!p.ok) break;
            // Duplicates and very old packets are dropped (replays too).
            if (c->anyReceived) {
                if (newer(sequence, c->remoteSequence)) {
                    const std::uint32_t shift = static_cast<std::uint16_t>(sequence - c->remoteSequence);
                    const std::uint64_t bits = shift >= 33 ? 0 : (static_cast<std::uint64_t>(c->receivedBits) << shift) | (1ull << (shift - 1));
                    c->receivedBits = static_cast<std::uint32_t>(bits);
                    c->remoteSequence = sequence;
                } else {
                    const std::uint32_t back = static_cast<std::uint16_t>(c->remoteSequence - sequence);
                    if (back == 0 || back > 32 || (c->receivedBits & (1u << (back - 1))) != 0) return;
                    c->receivedBits |= 1u << (back - 1);
                }
            } else {
                c->anyReceived = true;
                c->remoteSequence = sequence;
                c->receivedBits = 0;
            }
            // What the other side has received of ours.
            auto acknowledge = [&](std::uint16_t s) {
                if (const auto t = c->sentTimes.find(s); t != c->sentTimes.end()) {
                    c->roundTrip = c->roundTrip * 0.875 + (now - t->second) * 0.125;
                    c->sentTimes.erase(t);
                }
                if (const auto m = c->packetMessages.find(s); m != c->packetMessages.end()) {
                    for (const std::uint16_t messageId : m->second) {
                        std::erase_if(c->reliableOut, [&](const Connection::Outgoing& o) { return o.id == messageId; });
                    }
                    c->packetMessages.erase(m);
                }
            };
            if ((flags & kFlagAck) != 0) {
                acknowledge(ack);
                for (std::uint16_t i = 0; i < 32; ++i) {
                    if ((ackBits & (1u << i)) != 0) acknowledge(static_cast<std::uint16_t>(ack - 1 - i));
                }
            }
            // Messages.
            while (p.ok && p.at < plain.size()) {
                const auto channel = static_cast<Channel>(p.u8());
                const std::uint16_t messageId = channel == Channel::Reliable ? p.u16() : 0;
                const std::uint16_t length = p.u16();
                const auto body = p.bytes(length);
                if (!p.ok) break;
                c->ackPending = true;  // (packets without messages are not answered: no ack ping-pong)
                if (channel == Channel::Unreliable) {
                    events.push_back({NetEvent::Kind::Message, c->id, channel, std::vector<std::byte>(body.begin(), body.end()), {}});
                } else if (messageId == c->expectedReliable) {
                    events.push_back({NetEvent::Kind::Message, c->id, channel, std::vector<std::byte>(body.begin(), body.end()), {}});
                    ++c->expectedReliable;
                    for (auto early = c->reliableIn.find(c->expectedReliable); early != c->reliableIn.end();
                         early = c->reliableIn.find(c->expectedReliable)) {
                        events.push_back({NetEvent::Kind::Message, c->id, channel, std::move(early->second), {}});
                        c->reliableIn.erase(early);
                        ++c->expectedReliable;
                    }
                } else if (newer(messageId, c->expectedReliable) && static_cast<std::uint16_t>(messageId - c->expectedReliable) < 4096) {
                    c->reliableIn.try_emplace(messageId, body.begin(), body.end());
                }
            }
            if ((flags & kFlagDisconnect) != 0) {
                events.push_back({NetEvent::Kind::Disconnected, c->id, Channel::Reliable, {}, hosting_ ? "left" : "the host closed the game"});
                c->state = Connection::State::Handshake;
                const PeerId gone = c->id;
                std::erase_if(connections_, [&](const auto& x) { return x->id == gone; });
            }
            return;
        }
    }
    ++stats_.rejected;
}

void Transport::flush(Connection& c, double now) {
    const double resend = std::clamp(c.roundTrip * 1.5, 0.05, 1.0);
    for (int packet = 0; packet < kMaxPacketsPerFlush; ++packet) {
        std::vector<std::byte> plain;
        Writer p{plain};
        const std::uint16_t sequence = c.nextSequence;
        p.u16(sequence);
        p.u16(c.remoteSequence);
        p.u32(c.receivedBits);
        p.u8(c.anyReceived ? kFlagAck : 0);
        const std::size_t budget = kMaxDatagram - kHeader - kSeal;
        std::vector<std::uint16_t> carried;
        bool any = false;
        for (Connection::Outgoing& o : c.reliableOut) {
            if (o.lastSent >= 0.0 && now - o.lastSent < resend) continue;
            if (plain.size() + kMessageHeader + o.data.size() > budget) continue;
            if (o.lastSent >= 0.0) ++stats_.resent;
            p.u8(static_cast<std::uint8_t>(Channel::Reliable));
            p.u16(o.id);
            p.u16(static_cast<std::uint16_t>(o.data.size()));
            p.bytes(o.data);
            o.lastSent = now;
            carried.push_back(o.id);
            any = true;
        }
        while (!c.unreliableOut.empty() && plain.size() + kMessageHeader + c.unreliableOut.front().size() <= budget) {
            const auto& m = c.unreliableOut.front();
            p.u8(static_cast<std::uint8_t>(Channel::Unreliable));
            p.u16(static_cast<std::uint16_t>(m.size()));
            p.bytes(m);
            c.unreliableOut.pop_front();
            any = true;
        }
        if (!any && !c.ackPending && c.lastSent >= 0.0 && now - c.lastSent < kKeepAlive) break;
        // Seal and send.
        std::vector<std::byte> datagram;
        Writer w{datagram};
        writeHeader(w, PacketType::Data);
        w.u32(hosting_ ? c.id : localId_);
        std::array<std::uint8_t, 24> nonce{};
        randomBytes(nonce);
        std::array<std::uint8_t, 16> mac{};
        std::vector<std::byte> cipher(plain.size());
        crypto_aead_lock(reinterpret_cast<std::uint8_t*>(cipher.data()), mac.data(), c.sendKey.data(), nonce.data(),
                         reinterpret_cast<const std::uint8_t*>(datagram.data()), datagram.size(),
                         reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size());
        w.bytes(std::span<const std::uint8_t>(nonce));
        w.bytes(std::span<const std::uint8_t>(mac));
        w.bytes(std::span<const std::byte>(cipher));
        sendRaw(c.endpoint, datagram);
        c.sentTimes[sequence] = now;
        if (!carried.empty()) c.packetMessages[sequence] = std::move(carried);
        ++c.nextSequence;
        c.lastSent = now;
        c.ackPending = false;
        if (!any || (c.unreliableOut.empty() &&
                     std::ranges::none_of(c.reliableOut, [&](const Connection::Outgoing& o) { return o.lastSent < 0.0; }))) {
            break;
        }
    }
    // Forget packets too old to be acknowledged any more (their messages go out again anyway).
    while (!c.sentTimes.empty() && now - c.sentTimes.begin()->second > 5.0) c.sentTimes.erase(c.sentTimes.begin());
    while (c.packetMessages.size() > 1024) c.packetMessages.erase(c.packetMessages.begin());
}

} // namespace lumora::net
