#include "TestUtils.hpp"

#include "runtime/Simulation.hpp"
#include "runtime/net/NetSession.hpp"
#include "scene/Prefab.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <fstream>

using namespace lumora;

namespace {

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

/// Moves its entity along +x while this machine has authority over it.
constexpr const char* kMover = R"(
    local Mover = Behaviour:extend()
    function Mover:update(dt)
        if self:hasAuthority() then self:moveBy(vec3(self:param("speed", 1) * dt, 0, 0)) end
    end
    return Mover
)";

/// The same scene twice (same UUIDs), as two machines would load it from one file.
void buildArena(Scene& scene) {
    const entt::entity box = scene.createEntity("Box", Uuid(1001));
    scene.registry().emplace<NetworkIdentityComponent>(box);
    auto& script = scene.registry().emplace<ScriptComponent>(box);
    script.behaviour = "Scripts/Mover.lua";
    script.parameters = "speed=2";
    // Not shared: each machine runs its own.
    const entt::entity local = scene.createEntity("Spinner", Uuid(1002));
    scene.registry().emplace<ScriptComponent>(local).behaviour = "Scripts/Mover.lua";
    // A falling crate on the ground: simulated on the host only.
    const entt::entity ground = scene.createEntity("Ground", Uuid(1003));
    scene.registry().get<TransformComponent>(ground).position = {0.0f, -0.5f, 0.0f};
    scene.registry().emplace<ColliderComponent>(ground).size = {40.0f, 1.0f, 40.0f};
    const entt::entity crate = scene.createEntity("Crate", Uuid(1004));
    scene.registry().get<TransformComponent>(crate).position = {3.0f, 6.0f, 0.0f};
    scene.registry().emplace<ColliderComponent>(crate);
    scene.registry().emplace<RigidBodyComponent>(crate);
    scene.registry().emplace<NetworkIdentityComponent>(crate);
}

struct Machine {
    Scene scene;
    std::unique_ptr<Simulation> sim;
    void start(const std::filesystem::path& project) {
        sim = std::make_unique<Simulation>(scene);
        sim->setProjectRoot(project);
        sim->setScripts(nullptr, nullptr);
    }
    net::NetSession& net() { return sim->network(); }
    entt::entity find(const std::string& name) {
        for (auto [e, n] : scene.registry().view<NameComponent>().each()) {
            if (n.name == name) return e;
        }
        return entt::null;
    }
    glm::vec3 position(const std::string& name) { return scene.registry().get<TransformComponent>(find(name)).position; }
};

struct Game {
    test::TempDir project;
    Machine host, client;

    void step(int steps = 1) {
        for (int i = 0; i < steps; ++i) {
            host.sim->step();
            client.sim->step();
        }
    }
    template <class Done>
    bool until(Done done, int steps = 600) {
        for (int i = 0; i < steps; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }
    /// Hosts on a free port and joins it.
    void connect(const std::string& password = "pw") {
        ASSERT_TRUE(host.net().host(0, password).has_value());
        ASSERT_TRUE(client.net().join(std::format("127.0.0.1:{}", host.net().port()), password).has_value());
        ASSERT_TRUE(until([&] { return client.net().role() == net::NetRole::Client && !host.net().peers().empty(); }));
    }
};

} // namespace

TEST(NetGame, HostMovementShowsOnTheClient) {
    Game g;
    write(g.project.path() / "Scripts/Mover.lua", kMover);
    buildArena(g.host.scene);
    buildArena(g.client.scene);
    g.host.start(g.project.path());
    g.client.start(g.project.path());
    g.connect();

    EXPECT_EQ(g.host.net().role(), net::NetRole::Host);
    EXPECT_EQ(g.client.net().peers(), std::vector<net::PeerId>{net::kHostPeer});
    const net::PeerId me = g.client.net().localPeer();
    EXPECT_NE(me, net::kHostPeer);
    EXPECT_EQ(g.host.net().peers(), std::vector<net::PeerId>{me});
    const entt::entity clientBox = g.client.find("Box");
    ASSERT_TRUE(g.client.net().networked(clientBox));
    EXPECT_FALSE(g.client.net().hasAuthority(clientBox));
    EXPECT_TRUE(g.host.net().hasAuthority(g.host.find("Box")));
    EXPECT_FALSE(g.client.net().networked(g.client.find("Spinner")));

    // The host moves the box (2 m/s); the client sees it a little behind, smoothly.
    g.step(120);
    const float hostX = g.host.position("Box").x;
    const float clientX = g.client.position("Box").x;
    EXPECT_GT(hostX, 2.0f);
    EXPECT_GT(clientX, hostX - 0.6f);
    EXPECT_LE(clientX, hostX + 1e-3f);
    float previous = clientX;
    for (int i = 0; i < 20; ++i) {
        g.step();
        const float x = g.client.position("Box").x;
        EXPECT_GE(x, previous - 1e-4f) << "step " << i;  // never jumps back
        EXPECT_LT(x - previous, 0.2f) << "step " << i;   // nor ahead
        previous = x;
    }
    // The crate falls on the host; on the client it is kinematic and follows.
    const entt::entity clientCrate = g.client.find("Crate");
    EXPECT_EQ(g.client.scene.registry().get<RigidBodyComponent>(clientCrate).motion, BodyMotion::Kinematic);
    EXPECT_EQ(g.host.scene.registry().get<RigidBodyComponent>(g.host.find("Crate")).motion, BodyMotion::Dynamic);
    g.step(120);
    EXPECT_LT(g.host.position("Crate").y, 1.0f);
    EXPECT_NEAR(g.client.position("Crate").y, g.host.position("Crate").y, 0.1f);

    // The client leaves: everything is its own again.
    g.client.net().leave();
    EXPECT_EQ(g.client.net().role(), net::NetRole::Offline);
    EXPECT_EQ(g.client.scene.registry().get<RigidBodyComponent>(clientCrate).motion, BodyMotion::Dynamic);
    EXPECT_TRUE(g.client.net().hasAuthority(clientBox));
    ASSERT_TRUE(g.until([&] { return g.host.net().peers().empty(); }, 30));
}

TEST(NetGame, SpawnsOwnershipCallsAndValues) {
    Game g;
    const auto& root = g.project.path();
    // The host gives every player who joins a character; its owner moves it.
    write(root / "Scripts/Manager.lua", R"(
        local Manager = Behaviour:extend()
        function Manager:start() self.players = {} end
        function Manager:onPlayerJoined(player)
            if not self:isServer() then return end
            local p = self:spawn("Content/Prefabs/Player.lprefab", vec3(0, 0, 5))
            self:setOwner(p, player)
            self.players[player] = p
        end
        function Manager:onPlayerLeft(player)
            if self:isServer() and self.players[player] then self:destroy(self.players[player]) end
        end
        function Manager:onRpc(name, data, sender)
            if name == "hello" then self:setGameText("hello", data .. "@" .. sender) end
        end
        return Manager
    )");
    write(root / "Scripts/Player.lua", R"(
        local Player = Behaviour:extend()
        function Player:update(dt)
            if self:isClient() and self:hasAuthority() then
                self:moveBy(vec3(0, 0, -dt))
                self:setNetNumber("hp", 42)
            end
        end
        function Player:onRpc(name, data, sender)
            if name == "hit" then self.hits = (self.hits or 0) + 1; self:setNetText("lastHit", data) end
        end
        return Player
    )");
    {
        Scene source;
        const entt::entity player = source.createEntity("Player");
        source.registry().emplace<NetworkIdentityComponent>(player);
        source.registry().emplace<ScriptComponent>(player).behaviour = "Scripts/Player.lua";
        const entt::entity hat = source.createEntity("Hat", Uuid::generate(), player);
        source.registry().get<TransformComponent>(hat).position = {0.0f, 2.0f, 0.0f};
        ASSERT_TRUE(savePrefab(source, player, root / "Content/Prefabs/Player.lprefab").has_value());
    }
    for (Machine* m : {&g.host, &g.client}) {
        const entt::entity manager = m->scene.createEntity("Manager", Uuid(2001));
        m->scene.registry().emplace<ScriptComponent>(manager).behaviour = "Scripts/Manager.lua";
        m->start(root);
    }
    g.connect();
    const net::PeerId me = g.client.net().localPeer();

    // The character appears on the client (same UUIDs, children too), owned by it.
    ASSERT_TRUE(g.until([&] { return g.client.find("Player") != entt::null; }, 60));
    const entt::entity hostPlayer = g.host.find("Player");
    const entt::entity clientPlayer = g.client.find("Player");
    ASSERT_TRUE(hostPlayer != entt::null);
    EXPECT_EQ(g.client.scene.uuidOf(clientPlayer), g.host.scene.uuidOf(hostPlayer));
    EXPECT_EQ(g.client.scene.uuidOf(g.client.find("Hat")), g.host.scene.uuidOf(g.host.find("Hat")));
    EXPECT_EQ(g.host.net().owner(hostPlayer), me);
    EXPECT_EQ(g.client.net().owner(clientPlayer), me);
    EXPECT_TRUE(g.client.net().hasAuthority(clientPlayer));
    EXPECT_FALSE(g.host.net().hasAuthority(hostPlayer));

    // The client moves it; the host sees that (and the shared value).
    g.step(90);
    EXPECT_LT(g.client.position("Player").z, 4.0f);
    EXPECT_LT(g.host.position("Player").z, 4.5f);
    EXPECT_NEAR(g.host.position("Player").z, g.client.position("Player").z, 0.4f);
    const net::NetValue* hp = g.host.net().value(hostPlayer, "hp");
    ASSERT_NE(hp, nullptr);
    EXPECT_DOUBLE_EQ(hp->number, 42.0);
    // The host may not set values on what the client owns.
    EXPECT_FALSE(g.host.net().setValue(hostPlayer, "hp", {false, 1.0, {}}));

    // A call to the host, answered with a game value everyone sees.
    ASSERT_TRUE(g.client.net().rpc(entt::null, "hello", "hi", net::RpcTarget::Host));
    ASSERT_TRUE(g.until([&] {
        const net::NetValue* v = g.client.net().value(entt::null, "hello");
        return v != nullptr && v->text == std::format("hi@{}", me);
    }, 60));
    // A call on the character from the host runs where its owner is.
    ASSERT_TRUE(g.host.net().rpc(hostPlayer, "hit", "sword", net::RpcTarget::Owner));
    ASSERT_TRUE(g.until([&] {
        const net::NetValue* v = g.host.net().value(hostPlayer, "lastHit");
        return v != nullptr && v->text == "sword";
    }, 60));

    // Values and spawned entities reach a player who comes later.
    Machine late;
    (void)late.scene.createEntity("Manager", Uuid(2001));
    late.start(root);
    ASSERT_TRUE(late.net().join(std::format("127.0.0.1:{}", g.host.net().port()), "pw").has_value());
    ASSERT_TRUE(g.until([&] {
        late.sim->step();
        return late.find("Player") != entt::null && late.net().value(entt::null, "hello") != nullptr &&
               late.net().peers().size() == 2;  // the host and the first client
    }, 120));
    // Every player's character, each owned by its player.
    ASSERT_TRUE(g.until([&] {
        late.sim->step();
        std::vector<net::PeerId> owners;
        for (auto [e, n] : late.scene.registry().view<NameComponent>().each()) {
            if (n.name == "Player") owners.push_back(late.net().owner(e).value_or(99));
        }
        std::ranges::sort(owners);
        return owners == std::vector<net::PeerId>{me, late.net().localPeer()};
    }, 60));
    EXPECT_EQ(g.client.net().peers().size(), 2u);
    late.net().leave();
    ASSERT_TRUE(g.until([&] { return g.host.net().peers().size() == 1; }, 30));

    // The first client leaves: the host's manager removes its character, everywhere.
    g.client.net().leave();
    ASSERT_TRUE(g.until([&] { return g.host.find("Player") == entt::null || !g.host.scene.isValid(hostPlayer); }, 30));
    EXPECT_FALSE(g.host.scene.isValid(hostPlayer));
}

TEST(NetGame, ClientsFollowTheHostsSceneAndHearWhyGamesEnd) {
    Game g;
    const auto& root = g.project.path();
    write(root / "Scripts/Mover.lua", kMover);
    write(root / "Scripts/Watcher.lua", R"(
        local Watcher = Behaviour:extend()
        function Watcher:onConnected() self:setPosition(vec3(1, 0, 0)) end
        function Watcher:onDisconnected(reason)
            self:setPosition(vec3(2, 0, 0))
            self.reason = reason
        end
        return Watcher
    )");
    {
        Scene level;
        const entt::entity box = level.createEntity("Box", Uuid(3001));
        level.registry().emplace<NetworkIdentityComponent>(box);
        auto& script = level.registry().emplace<ScriptComponent>(box);
        script.behaviour = "Scripts/Mover.lua";
        script.parameters = "speed=3";
        (void)level.createEntity("Gone", Uuid(3002));  // only in this level
        ASSERT_TRUE(SceneSerializer::save(level, root / "Scenes/Level.lscene").has_value());
    }
    for (Machine* m : {&g.host, &g.client}) {
        const entt::entity watcher = m->scene.createEntity("Watcher", Uuid(3100));
        m->scene.registry().emplace<ScriptComponent>(watcher).behaviour = "Scripts/Watcher.lua";
        m->start(root);
    }
    g.connect();
    g.step(2);
    EXPECT_EQ(g.client.position("Watcher").x, 1.0f);  // onConnected ran

    // A client may not change scenes; the host takes everyone along.
    ASSERT_TRUE(g.host.sim->loadScene("Scenes/Level.lscene").has_value());
    ASSERT_TRUE(g.until([&] { return g.client.sim->loadedScene() == "Scenes/Level.lscene"; }, 30));
    EXPECT_TRUE(g.client.find("Watcher") == entt::null);
    g.step(60);
    EXPECT_GT(g.host.position("Box").x, 1.0f);
    EXPECT_GT(g.client.position("Box").x, 0.5f);
    EXPECT_TRUE(g.client.net().networked(g.client.find("Box")));

    // The host closes the game: the client hears why.
    g.host.net().leave();
    ASSERT_TRUE(g.until([&] { return g.client.net().role() == net::NetRole::Offline; }, 30));
    EXPECT_EQ(g.client.net().lastError(), "the host closed the game");
}

TEST(NetGame, WrongPasswordAndOfflineCalls) {
    Game g;
    const auto& root = g.project.path();
    write(root / "Scripts/Echo.lua", R"(
        local Echo = Behaviour:extend()
        function Echo:onRpc(name, data, sender) self:moveBy(vec3(tonumber(data), 0, 0)) end
        return Echo
    )");
    for (Machine* m : {&g.host, &g.client}) {
        const entt::entity e = m->scene.createEntity("Echo", Uuid(4001));
        m->scene.registry().emplace<ScriptComponent>(e).behaviour = "Scripts/Echo.lua";
        m->start(root);
    }
    // Offline, a call to everyone runs here (next step); to the others it goes nowhere.
    const entt::entity echo = g.host.find("Echo");
    EXPECT_TRUE(g.host.net().rpc(echo, "move", "2", net::RpcTarget::All));
    EXPECT_FALSE(g.host.net().rpc(echo, "move", "5", net::RpcTarget::Others));
    g.host.sim->step();
    EXPECT_FLOAT_EQ(g.host.position("Echo").x, 2.0f);
    EXPECT_TRUE(g.host.net().hasAuthority(echo));
    EXPECT_TRUE(g.host.net().setValue(entt::null, "round", {false, 3.0, {}}));

    ASSERT_TRUE(g.host.net().host(0, "right").has_value());
    ASSERT_TRUE(g.client.net().join(std::format("127.0.0.1:{}", g.host.net().port()), "wrong").has_value());
    ASSERT_TRUE(g.until([&] { return g.client.net().role() == net::NetRole::Offline; }, 120));
    EXPECT_EQ(g.client.net().lastError(), "wrong password");
    EXPECT_TRUE(g.host.net().peers().empty());
    EXPECT_FALSE(g.client.net().join("not an address", "").has_value());
}
