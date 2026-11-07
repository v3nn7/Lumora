#include "runtime/net/Socket.hpp"
#include "runtime/net/Transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

using namespace lumora;
using namespace lumora::net;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::string text(const std::vector<std::byte>& data) { return {reinterpret_cast<const char*>(data.data()), data.size()}; }

/// Polls every transport with a simulated clock (loopback delivers at once).
struct Loop {
    double now = 0.0;
    std::vector<NetEvent> hostEvents, clientEvents, otherEvents;

    void step(Transport& host, Transport* client, Transport* other = nullptr, double dt = 0.01) {
        now += dt;
        for (auto& e : host.poll(now)) hostEvents.push_back(std::move(e));
        if (client != nullptr) {
            for (auto& e : client->poll(now)) clientEvents.push_back(std::move(e));
        }
        if (other != nullptr) {
            for (auto& e : other->poll(now)) otherEvents.push_back(std::move(e));
        }
        std::this_thread::yield();
    }
    template <class Done>
    bool until(Transport& host, Transport* client, Done done, int steps = 2000, Transport* other = nullptr) {
        for (int i = 0; i < steps; ++i) {
            if (done()) return true;
            step(host, client, other);
        }
        return done();
    }
};

bool has(const std::vector<NetEvent>& events, NetEvent::Kind kind) {
    return std::ranges::any_of(events, [&](const NetEvent& e) { return e.kind == kind; });
}

std::vector<std::string> messages(const std::vector<NetEvent>& events, Channel channel) {
    std::vector<std::string> out;
    for (const auto& e : events) {
        if (e.kind == NetEvent::Kind::Message && e.channel == channel) out.push_back(text(e.data));
    }
    return out;
}

HostPort loopback(const Transport& host) { return {"127.0.0.1", host.port()}; }

} // namespace

TEST(Net, ParsesAddresses) {
    auto a = parseHostPort("192.168.1.20:7000");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->host, "192.168.1.20");
    EXPECT_EQ(a->port, 7000);
    a = parseHostPort("  lumora://name.at.ply.gg:12345/ ");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->host, "name.at.ply.gg");
    EXPECT_EQ(a->port, 12345);
    a = parseHostPort("[::1]:99");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->host, "::1");
    EXPECT_EQ(a->port, 99);
    EXPECT_EQ(formatHostPort(*a), "[::1]:99");
    a = parseHostPort("localhost");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->port, kDefaultPort);
    EXPECT_FALSE(parseHostPort(""));
    EXPECT_FALSE(parseHostPort("host:70000"));
    EXPECT_FALSE(parseHostPort("host:"));
    EXPECT_FALSE(parseHostPort("[::1"));
}

TEST(Net, ConnectsAndExchangesMessages) {
    auto host = Transport::host(0, "secret");
    ASSERT_TRUE(host) << host.error().message;
    auto client = Transport::connect(loopback(**host), "secret");
    ASSERT_TRUE(client) << client.error().message;
    Loop loop;
    ASSERT_TRUE(loop.until(**host, client->get(), [&] { return (*client)->connected() && !(*host)->peers().empty(); }));
    EXPECT_TRUE(has(loop.clientEvents, NetEvent::Kind::Connected));
    EXPECT_TRUE(has(loop.hostEvents, NetEvent::Kind::Connected));
    const PeerId id = (*client)->localId();
    EXPECT_NE(id, kHostPeer);
    EXPECT_EQ((*host)->peers(), std::vector<PeerId>{id});

    EXPECT_TRUE((*client)->send(kHostPeer, Channel::Reliable, bytes("hello host")));
    EXPECT_TRUE((*client)->send(kHostPeer, Channel::Unreliable, bytes("position")));
    EXPECT_TRUE((*host)->send(id, Channel::Reliable, bytes("welcome, player")));
    (*host)->broadcast(Channel::Unreliable, bytes("snapshot"));
    loop.until(**host, client->get(), [&] { return loop.hostEvents.size() >= 3 && loop.clientEvents.size() >= 3; }, 50);
    EXPECT_EQ(messages(loop.hostEvents, Channel::Reliable), std::vector<std::string>{"hello host"});
    EXPECT_EQ(messages(loop.hostEvents, Channel::Unreliable), std::vector<std::string>{"position"});
    EXPECT_EQ(messages(loop.clientEvents, Channel::Reliable), std::vector<std::string>{"welcome, player"});
    EXPECT_EQ(messages(loop.clientEvents, Channel::Unreliable), std::vector<std::string>{"snapshot"});
    for (const auto& e : loop.hostEvents) {
        if (e.kind == NetEvent::Kind::Message) {
            EXPECT_EQ(e.peer, id);
        }
    }
    // Too big, or nobody there.
    EXPECT_FALSE((*host)->send(id, Channel::Reliable, std::vector<std::byte>(kMaxMessage + 1)));
    EXPECT_FALSE((*host)->send(id + 7, Channel::Reliable, bytes("x")));
    EXPECT_GT((*client)->stats().roundTrip, 0.0);
}

TEST(Net, ReliableMessagesArriveInOrderDespiteLoss) {
    auto host = Transport::host(0, "");
    auto client = Transport::connect(loopback(**host), "");
    ASSERT_TRUE(host && client);
    Loop loop;
    ASSERT_TRUE(loop.until(**host, client->get(), [&] { return (*client)->connected() && !(*host)->peers().empty(); }));
    (*host)->simulatePacketLoss(0.3f, 7);
    (*client)->simulatePacketLoss(0.3f, 11);
    const PeerId id = (*client)->localId();
    constexpr int kCount = 300;
    for (int i = 0; i < kCount; ++i) {
        const std::string body = std::format("message {} {}", i, std::string(static_cast<std::size_t>(i % 40) * 20, '#'));
        ASSERT_TRUE((*host)->send(id, Channel::Reliable, bytes(body)));
        ASSERT_TRUE((*client)->send(kHostPeer, Channel::Reliable, bytes(body)));
    }
    ASSERT_TRUE(loop.until(**host, client->get(), [&] {
        return messages(loop.clientEvents, Channel::Reliable).size() >= kCount && messages(loop.hostEvents, Channel::Reliable).size() >= kCount;
    }, 5000));
    const auto toClient = messages(loop.clientEvents, Channel::Reliable);
    const auto toHost = messages(loop.hostEvents, Channel::Reliable);
    ASSERT_EQ(toClient.size(), static_cast<std::size_t>(kCount));
    ASSERT_EQ(toHost.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_TRUE(toClient[static_cast<std::size_t>(i)].starts_with(std::format("message {} ", i)));
        EXPECT_TRUE(toHost[static_cast<std::size_t>(i)].starts_with(std::format("message {} ", i)));
    }
    EXPECT_GT((*host)->stats().resent, 0u);
    EXPECT_FALSE(has(loop.clientEvents, NetEvent::Kind::Disconnected));
}

TEST(Net, WrongPasswordCannotConnect) {
    auto host = Transport::host(0, "right");
    auto client = Transport::connect(loopback(**host), "wrong");
    ASSERT_TRUE(host && client);
    Loop loop;
    ASSERT_TRUE(loop.until(**host, client->get(), [&] { return has(loop.clientEvents, NetEvent::Kind::Disconnected); }, 200));
    EXPECT_EQ(loop.clientEvents.front().reason, "wrong password");
    EXPECT_FALSE((*client)->connected());
    EXPECT_TRUE((*host)->peers().empty());
    EXPECT_FALSE(has(loop.hostEvents, NetEvent::Kind::Connected));
    // The half-open attempt is forgotten.
    for (int i = 0; i < 1200; ++i) loop.step(**host, nullptr);
    EXPECT_TRUE((*host)->peers().empty());
}

TEST(Net, IgnoresForgedAndAlteredDatagrams) {
    auto host = Transport::host(0, "pw");
    auto client = Transport::connect(loopback(**host), "pw");
    ASSERT_TRUE(host && client);
    Loop loop;
    ASSERT_TRUE(loop.until(**host, client->get(), [&] { return (*client)->connected() && !(*host)->peers().empty(); }));
    const PeerId id = (*client)->localId();
    auto endpoint = resolve(loopback(**host));
    ASSERT_TRUE(endpoint);
    auto attacker = UdpSocket::open(*endpoint);
    ASSERT_TRUE(attacker);
    const auto before = (*host)->stats().rejected;
    // Garbage, a short Hello (no amplification), and a data packet for the client's id with a
    // made-up body.
    attacker->send(*endpoint, bytes("not a lumora packet"));
    attacker->send(*endpoint, bytes("LMNT\x01\x01short"));
    std::string forged = "LMNT";
    forged += '\x01';
    forged += '\x04';
    forged += std::string(reinterpret_cast<const char*>(&id), 4);
    forged += std::string(64, 'A');
    attacker->send(*endpoint, bytes(forged));
    for (int i = 0; i < 5; ++i) loop.step(**host, client->get());
    EXPECT_GE((*host)->stats().rejected, before + 3);
    EXPECT_TRUE(messages(loop.hostEvents, Channel::Reliable).empty());
    EXPECT_TRUE(messages(loop.hostEvents, Channel::Unreliable).empty());
    std::array<std::byte, 256> answer{};
    Endpoint from;
    EXPECT_FALSE(attacker->receive(answer, from));  // nothing comes back
    // The real client still works.
    (*client)->send(kHostPeer, Channel::Reliable, bytes("still here"));
    loop.until(**host, client->get(), [&] { return !messages(loop.hostEvents, Channel::Reliable).empty(); }, 50);
    EXPECT_EQ(messages(loop.hostEvents, Channel::Reliable), std::vector<std::string>{"still here"});
}

TEST(Net, DisconnectsAndTimesOut) {
    auto host = Transport::host(0, "");
    ASSERT_TRUE(host);
    Loop loop;
    {
        auto client = Transport::connect(loopback(**host), "");
        ASSERT_TRUE(client);
        ASSERT_TRUE(loop.until(**host, client->get(), [&] { return (*client)->connected() && !(*host)->peers().empty(); }));
        (*client)->disconnect(kHostPeer);
        EXPECT_FALSE((*client)->connected());
        loop.until(**host, client->get(), [&] { return has(loop.hostEvents, NetEvent::Kind::Disconnected); }, 20);
        ASSERT_TRUE(has(loop.hostEvents, NetEvent::Kind::Disconnected));
        EXPECT_EQ(loop.hostEvents.back().reason, "left");
        EXPECT_TRUE((*host)->peers().empty());
    }
    // A client that goes silent.
    loop.hostEvents.clear();
    loop.clientEvents.clear();
    auto client = Transport::connect(loopback(**host), "");
    ASSERT_TRUE(client);
    ASSERT_TRUE(loop.until(**host, client->get(), [&] { return (*client)->connected() && !(*host)->peers().empty(); }));
    (*client)->simulatePacketLoss(1.0f);
    (*host)->simulatePacketLoss(1.0f);
    for (int i = 0; i < 120; ++i) loop.step(**host, client->get(), nullptr, 0.1);
    ASSERT_TRUE(has(loop.hostEvents, NetEvent::Kind::Disconnected));
    ASSERT_TRUE(has(loop.clientEvents, NetEvent::Kind::Disconnected));
    EXPECT_EQ(loop.hostEvents.back().reason, "timed out");
    EXPECT_EQ(loop.clientEvents.back().reason, "timed out");
    EXPECT_TRUE((*host)->peers().empty());
    EXPECT_FALSE((*client)->connected());
}

TEST(Net, FullHostRejectsAndClosedHostIsNoticed) {
    auto host = Transport::host(0, "", 1);
    auto first = Transport::connect(loopback(**host), "");
    ASSERT_TRUE(host && first);
    Loop loop;
    ASSERT_TRUE(loop.until(**host, first->get(), [&] { return (*first)->connected() && !(*host)->peers().empty(); }));
    auto second = Transport::connect(loopback(**host), "");
    ASSERT_TRUE(second);
    ASSERT_TRUE(loop.until(**host, first->get(), [&] { return has(loop.otherEvents, NetEvent::Kind::Disconnected); }, 200, second->get()));
    EXPECT_EQ(loop.otherEvents.back().reason, "the game is full");
    EXPECT_EQ((*host)->peers().size(), 1u);
    // The host shuts down: the client hears it at once.
    host->reset();
    for (int i = 0; i < 5; ++i) {
        loop.now += 0.01;
        for (auto& e : (*first)->poll(loop.now)) loop.clientEvents.push_back(std::move(e));
    }
    ASSERT_TRUE(has(loop.clientEvents, NetEvent::Kind::Disconnected));
    EXPECT_EQ(loop.clientEvents.back().reason, "the host closed the game");
}
