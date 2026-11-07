#pragma once

#include "core/Error.hpp"
#include "core/Uuid.hpp"
#include "runtime/net/Transport.hpp"

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json_fwd.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lumora {
class Scene;
class PhysicsWorld;
}

namespace lumora::net {

/// Where an entity made during play came from (the scripts' spawn and clone set it), so the
/// clients can make the same one. Kept only while the game runs.
struct SpawnOrigin {
    std::string prefab;  // spawn(prefab)
    Uuid source;         // clone(source)
};

/// A value behaviours share with every player (setNetNumber / setNetText).
struct NetValue {
    bool isText = false;
    double number = 0.0;
    std::string text;
    friend bool operator==(const NetValue&, const NetValue&) = default;
};
/// The shared values of an entity (kept only while the game runs).
struct NetValuesComponent {
    std::map<std::string, NetValue, std::less<>> values;
};

enum class NetRole { Offline, Connecting, Host, Client };
/// Who runs an RPC.
enum class RpcTarget : std::uint8_t {
    Host = 0,    // the host
    All = 1,     // everyone, the caller too
    Others = 2,  // everyone but the caller
    Owner = 3,   // whoever owns the entity (the host when nobody does)
};

/// What the session tells behaviours.
struct NetScriptEvent {
    enum class Kind { Rpc = 0, PeerJoined = 1, PeerLeft = 2, Connected = 3, Disconnected = 4 };
    Kind kind = Kind::Rpc;
    entt::entity entity = entt::null;  // Rpc: the entity it was called on (null: the whole game)
    PeerId peer = kHostPeer;           // Rpc: who called it; PeerJoined / PeerLeft: who
    std::string name;                  // Rpc
    std::string data;                  // Rpc data; Disconnected: why
};

/// A network game of one simulation: the host (authoritative; it also plays) or a client.
///
/// - Entities with a NetworkIdentity component are shared. The scene's own are matched by UUID
///   (both sides load the same scene file; clients follow the host's scene changes); entities
///   the host's behaviours spawn or clone during play are made on the clients the same way.
///   Destroying one on the host destroys it everywhere.
/// - Every entity has an owner: the host, or a client the host gave it to (setOwner, e.g. a
///   player's character). Whoever owns it has authority: it moves it, and its transform goes to
///   everyone else about 30 times a second, played back smoothly a little behind. Dynamic
///   bodies become kinematic where they are not simulated.
/// - Behaviours call RPCs on entities (or the whole game) and share values (numbers, texts).
/// - Offline, the same calls work on this machine alone (an RPC to All runs here), so a game
///   runs single-player unchanged.
class NetSession {
public:
    struct Hooks {
        std::function<bool(const std::string& scene)> loadScene;  // clients follow the host's scene
        std::function<void(const NetScriptEvent&)> deliver;        // to behaviours
        std::function<PhysicsWorld*()> physics;
    };
    NetSession(Scene& scene, Hooks hooks);
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    /// Starts hosting on a port (0: any free one) for up to `maxClients` players.
    Result<> host(std::uint16_t port, std::string password, int maxClients = 16);
    /// Starts joining a host ("192.168.1.20:7778", "name.at.ply.gg:12345"); Connected or
    /// Disconnected follows.
    Result<> join(const std::string& address, std::string password);
    /// Ends the game (clients leave; a host closes it for everyone).
    void leave();

    [[nodiscard]] NetRole role() const noexcept { return role_; }
    /// Offline or hosting: the game's rules run here.
    [[nodiscard]] bool isServer() const noexcept { return role_ == NetRole::Offline || role_ == NetRole::Host; }
    /// This player's peer id: the host is 0, clients get theirs on connecting.
    [[nodiscard]] PeerId localPeer() const noexcept;
    /// The other players (host: its clients; client: the host and the other clients).
    [[nodiscard]] std::vector<PeerId> peers() const;
    [[nodiscard]] double roundTrip() const;
    [[nodiscard]] std::uint16_t port() const;
    /// Why the last game ended or could not start.
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

    [[nodiscard]] bool networked(entt::entity entity) const;
    /// The owner of a shared entity.
    [[nodiscard]] std::optional<PeerId> owner(entt::entity entity) const;
    /// Hosts: gives an entity to a player (kHostPeer: back to the host). Also right after spawning it.
    bool setOwner(entt::entity entity, PeerId peer);
    /// Whether this machine moves the entity (true for everything not shared).
    [[nodiscard]] bool hasAuthority(entt::entity entity) const;

    /// Calls `name` on the entity's behaviours (null: every behaviour) where `target` says, with
    /// any bytes as data. Runs at the start of the next step. False if it cannot be sent.
    bool rpc(entt::entity entity, std::string name, std::string data, RpcTarget target);
    /// Sets a shared value of an entity (null: of the whole game, hosts only); only who has
    /// authority over the entity may. False otherwise.
    bool setValue(entt::entity entity, std::string key, NetValue value);
    [[nodiscard]] const NetValue* value(entt::entity entity, std::string_view key) const;

    /// Start of a simulation step: what arrived (scene changes, spawns, calls, states).
    void beginStep(double dt, double simulatedSeconds);
    /// End of a step (after physics): new and removed entities, updates sent, smoothing.
    void endStep(double dt, double simulatedSeconds);
    /// The scene was replaced (Simulation::loadScene); `path` is project-relative.
    void sceneLoaded(const std::string& path);
    /// The scene the game started with (project-relative; clients load the host's).
    void setScenePath(std::string path) { scenePath_ = std::move(path); }
    void setProjectRoot(std::filesystem::path root) { projectRoot_ = std::move(root); }

    /// Seconds between updates sent (default 1/30).
    void setSendInterval(double seconds) noexcept { sendInterval_ = seconds > 0.0 ? seconds : sendInterval_; }
    /// How far behind smoothing plays received movement (default 0.1 s).
    void setInterpolationDelay(double seconds) noexcept { delay_ = seconds >= 0.0 ? seconds : delay_; }
    /// The clock the connection runs on (seconds; a steady clock by default).
    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }
    [[nodiscard]] Transport* transport() noexcept { return transport_.get(); }

    struct Pose {
        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};  // Euler degrees, as TransformComponent
        glm::vec3 scale{1.0f};
    };

private:
    struct Sample {
        double time;
        Pose pose;
    };
    struct Replica {
        std::uint32_t id = 0;
        entt::entity entity = entt::null;
        PeerId owner = kHostPeer;
        bool spawned = false;      // the root of a tree made during play (a Spawn)
        bool kinematic = false;    // its dynamic body was made kinematic here
        Pose sent;                 // what the last update carried
        double sentAt = -1.0;
        std::deque<Sample> samples;  // received, oldest first
    };
    struct Clock {
        double latest = 0.0;
        double play = 0.0;
        bool started = false;
    };
    struct OutgoingRpc {
        entt::entity entity;
        std::string name;
        std::string data;
        RpcTarget target;
    };

    [[nodiscard]] double now() const;
    void handle(const NetEvent& event, double simTime);
    void handleMessage(PeerId from, std::span<const std::byte> data, double simTime);
    void deliver(NetScriptEvent event);
    void resetReplication();
    void becomeOffline(const std::string& reason, bool tellScripts);
    void registerSceneEntities();
    void discoverEntities();
    void sendWorld(PeerId peer);
    void sendPeers();
    void sendSpawn(const Replica& root, PeerId to);
    [[nodiscard]] std::vector<std::vector<std::byte>> bindMessages(const std::vector<const Replica*>& replicas) const;
    void sendValues(const Replica& replica, PeerId to);
    void sendValue(std::uint32_t id, const std::string& key, const NetValue& value, PeerId to, PeerId except = ~PeerId{0});
    void sendStates(double simTime);
    void sendRpcs();
    void routeRpc(PeerId sender, std::uint32_t id, const std::string& name, const std::string& data, RpcTarget target);
    void applyAuthority(Replica& replica);
    void smooth(double dt);
    [[nodiscard]] Replica* replicaOf(entt::entity entity);
    [[nodiscard]] const Replica* replicaOf(entt::entity entity) const;
    [[nodiscard]] Replica* replica(std::uint32_t id);
    Replica& add(entt::entity entity, std::uint32_t id, PeerId owner, bool spawned);
    void pushSample(Replica& replica, PeerId source, double time, const Pose& pose);
    [[nodiscard]] Pose poseOf(entt::entity entity) const;
    void setPose(entt::entity entity, const Pose& pose);
    entt::entity makeSpawn(std::uint8_t kind, const std::string& prefab, Uuid source);
    void send(PeerId peer, Channel channel, const std::vector<std::byte>& message);
    void broadcast(Channel channel, const std::vector<std::byte>& message, PeerId except = ~PeerId{0});

    Scene& scene_;
    Hooks hooks_;
    NetRole role_ = NetRole::Offline;
    std::unique_ptr<Transport> transport_;
    std::function<double()> clock_;
    std::string lastError_;
    std::string scenePath_;
    std::filesystem::path projectRoot_;
    std::uint32_t epoch_ = 1;           // the host's scene number: messages of older scenes are dropped
    std::uint32_t nextId_ = 1;
    std::unordered_map<std::uint32_t, Replica> replicas_;
    std::unordered_map<entt::entity, std::uint32_t> ids_;
    std::unordered_set<entt::entity> localOnly_;       // host: shared entities it cannot share (logged once)
    std::unordered_set<entt::entity> unbound_;         // client: scene entities the host has not matched yet
    std::unordered_map<entt::entity, PeerId> pendingOwners_;  // host: owners of entities not shared yet
    std::unordered_map<PeerId, Clock> clocks_;         // per sender of movement
    std::vector<PeerId> remotePeers_;                  // client: the other players
    std::vector<PeerId> clients_;                      // host: connected clients, in joining order
    std::vector<NetEvent> pending_;                    // arrived at the end of the last step
    std::vector<NetScriptEvent> localEvents_;          // RPCs that run here, next step
    std::vector<OutgoingRpc> outgoing_;
    std::map<std::string, NetValue, std::less<>> globals_;
    std::unordered_map<std::string, std::shared_ptr<const nlohmann::json>> prefabs_;
    double sendInterval_ = 1.0 / 30.0;
    double sendTimer_ = 0.0;
    double delay_ = 0.1;
};

} // namespace lumora::net
