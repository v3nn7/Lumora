#include "runtime/net/NetSession.hpp"

#include "core/Log.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "runtime/net/Bytes.hpp"
#include "scene/Components.hpp"
#include "scene/Prefab.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace lumora::net {

namespace {

// Messages between the session's host and clients (first byte), all with the scene epoch next.
enum class Msg : std::uint8_t {
    Scene = 1,       // host -> client: the scene to be in (path)
    Bind = 2,        // host -> client: scene entities matched by UUID (id, uuid, owner, pose)
    BindDone = 3,    // host -> client: scene entities not matched are gone on the host
    Spawn = 4,       // host -> client: an entity made during play
    Despawn = 5,     // host -> client
    Owner = 6,       // host -> client
    Snapshot = 7,    // host -> client, unreliable: poses
    OwnerState = 8,  // client -> host, unreliable: poses of what the client owns
    Rpc = 9,         // both ways
    Value = 10,      // both ways
    Peers = 11,      // host -> client: who plays
};

constexpr std::size_t kBindEntry = 4 + 8 + 4 + 36;
constexpr std::size_t kStateEntry = 4 + 1 + 36;
constexpr std::size_t kMessageHeader = 1 + 4;
constexpr double kRefresh = 1.0;  // unchanged entities still go out this often (lost updates heal)
constexpr std::uint8_t kSpawnPrefab = 0;
constexpr std::uint8_t kSpawnClone = 1;

using Pose = NetSession::Pose;

void writePose(ByteWriter& w, const Pose& p) {
    for (const glm::vec3& v : {p.position, p.rotation, p.scale}) {
        w.f32(v.x);
        w.f32(v.y);
        w.f32(v.z);
    }
}

Pose readPose(ByteReader& r) {
    Pose p;
    for (glm::vec3* v : {&p.position, &p.rotation, &p.scale}) {
        v->x = r.f32();
        v->y = r.f32();
        v->z = r.f32();
    }
    return p;
}

bool finite(const Pose& p) {
    for (const glm::vec3& v : {p.position, p.rotation, p.scale}) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    }
    return true;
}

bool same(const Pose& a, const Pose& b) {
    auto near = [](glm::vec3 x, glm::vec3 y, float eps) {
        return std::abs(x.x - y.x) <= eps && std::abs(x.y - y.y) <= eps && std::abs(x.z - y.z) <= eps;
    };
    return near(a.position, b.position, 1e-4f) && near(a.rotation, b.rotation, 1e-3f) && near(a.scale, b.scale, 1e-4f);
}

glm::quat toQuat(glm::vec3 degrees) {
    const glm::vec3 r = glm::radians(degrees);
    return glm::quat_cast(glm::eulerAngleYXZ(r.y, r.x, r.z));
}

glm::vec3 toEuler(const glm::quat& q) {
    float y = 0.0f, x = 0.0f, z = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(q), y, x, z);
    return glm::degrees(glm::vec3(x, y, z));
}

Pose mix(const Pose& a, const Pose& b, float t) {
    Pose p;
    p.position = glm::mix(a.position, b.position, t);
    p.scale = glm::mix(a.scale, b.scale, t);
    p.rotation = a.rotation == b.rotation ? a.rotation : toEuler(glm::slerp(toQuat(a.rotation), toQuat(b.rotation), t));
    return p;
}

std::vector<std::byte> start(Msg type, std::uint32_t epoch) {
    std::vector<std::byte> out;
    out.reserve(64);
    ByteWriter w{out};
    w.u8(static_cast<std::uint8_t>(type));
    w.u32(epoch);
    return out;
}

void writeValue(ByteWriter& w, const NetValue& v) {
    w.u8(v.isText ? 1 : 0);
    if (v.isText) {
        w.str(v.text);
    } else {
        w.f64(v.number);
    }
}

NetValue readValue(ByteReader& r) {
    NetValue v;
    v.isText = r.u8() != 0;
    if (v.isText) {
        v.text = r.str();
    } else {
        v.number = r.f64();
    }
    return v;
}

} // namespace

NetSession::NetSession(Scene& scene, Hooks hooks) : scene_(scene), hooks_(std::move(hooks)) {
    clock_ = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
}

NetSession::~NetSession() {
    if (transport_) becomeOffline("closed", false);
}

double NetSession::now() const { return clock_(); }

Result<> NetSession::host(std::uint16_t port, std::string password, int maxClients) {
    if (role_ != NetRole::Offline) leave();
    auto transport = Transport::host(port, std::move(password), maxClients);
    if (!transport) {
        lastError_ = transport.error().message;
        return std::unexpected(transport.error());
    }
    transport_ = std::move(*transport);
    role_ = NetRole::Host;
    lastError_.clear();
    resetReplication();
    ++epoch_;
    registerSceneEntities();
    log::info("Net", "hosting a game on port {}", transport_->port());
    return {};
}

Result<> NetSession::join(const std::string& address, std::string password) {
    if (role_ != NetRole::Offline) leave();
    const auto parsed = parseHostPort(address);
    if (!parsed) {
        lastError_ = std::format("'{}' is not an address (host:port)", address);
        return makeError("{}", lastError_);
    }
    auto transport = Transport::connect(*parsed, std::move(password));
    if (!transport) {
        lastError_ = transport.error().message;
        return std::unexpected(transport.error());
    }
    transport_ = std::move(*transport);
    role_ = NetRole::Connecting;
    lastError_.clear();
    resetReplication();
    log::info("Net", "joining {}", formatHostPort(*parsed));
    return {};
}

void NetSession::leave() {
    if (transport_) becomeOffline("left", false);
}

void NetSession::becomeOffline(const std::string& reason, bool tellScripts) {
    const bool wasClient = role_ != NetRole::Host;
    resetReplication();  // dynamic bodies made kinematic simulate here again
    transport_.reset();  // a host says goodbye to everyone
    role_ = NetRole::Offline;
    lastError_ = reason;
    remotePeers_.clear();
    clients_.clear();
    pending_.clear();
    if (wasClient) globals_.clear();
    log::info("Net", "left the network game ({})", reason);
    if (tellScripts) localEvents_.push_back({NetScriptEvent::Kind::Disconnected, entt::null, kHostPeer, {}, reason});
}

PeerId NetSession::localPeer() const noexcept {
    return role_ == NetRole::Client && transport_ ? transport_->localId() : kHostPeer;
}

std::vector<PeerId> NetSession::peers() const {
    if (role_ == NetRole::Host) return clients_;
    if (role_ == NetRole::Client) return remotePeers_;
    return {};
}

double NetSession::roundTrip() const { return transport_ ? transport_->stats().roundTrip : 0.0; }

std::uint16_t NetSession::port() const { return transport_ ? transport_->port() : 0; }

// ---- Entities -----------------------------------------------------------------------------------

NetSession::Replica* NetSession::replicaOf(entt::entity entity) {
    const auto it = ids_.find(entity);
    return it == ids_.end() ? nullptr : replica(it->second);
}

const NetSession::Replica* NetSession::replicaOf(entt::entity entity) const {
    const auto it = ids_.find(entity);
    if (it == ids_.end()) return nullptr;
    const auto r = replicas_.find(it->second);
    return r == replicas_.end() ? nullptr : &r->second;
}

NetSession::Replica* NetSession::replica(std::uint32_t id) {
    const auto it = replicas_.find(id);
    return it == replicas_.end() ? nullptr : &it->second;
}

NetSession::Replica& NetSession::add(entt::entity entity, std::uint32_t id, PeerId owner, bool spawned) {
    if (const auto old = replicas_.find(id); old != replicas_.end()) ids_.erase(old->second.entity);
    Replica& r = replicas_[id];
    r = Replica{};
    r.id = id;
    r.entity = entity;
    r.owner = owner;
    r.spawned = spawned;
    ids_[entity] = id;
    return r;
}

bool NetSession::networked(entt::entity entity) const { return ids_.contains(entity); }

std::optional<PeerId> NetSession::owner(entt::entity entity) const {
    if (const Replica* r = replicaOf(entity)) return r->owner;
    if (role_ == NetRole::Host) {
        if (const auto it = pendingOwners_.find(entity); it != pendingOwners_.end()) return it->second;
    }
    return std::nullopt;
}

bool NetSession::hasAuthority(entt::entity entity) const {
    if (role_ == NetRole::Offline || role_ == NetRole::Connecting) return true;
    if (const Replica* r = replicaOf(entity)) return r->owner == localPeer();
    if (role_ == NetRole::Host) {
        if (const auto it = pendingOwners_.find(entity); it != pendingOwners_.end()) return it->second == kHostPeer;
    }
    return true;  // not shared: only here
}

bool NetSession::setOwner(entt::entity entity, PeerId peer) {
    if (role_ != NetRole::Host || !scene_.isValid(entity)) return false;
    if (peer != kHostPeer && std::ranges::find(clients_, peer) == clients_.end()) return false;
    if (Replica* r = replicaOf(entity)) {
        if (r->owner == peer) return true;
        r->owner = peer;
        r->samples.clear();
        r->sentAt = -1.0;  // the next update carries it
        auto message = start(Msg::Owner, epoch_);
        ByteWriter w{message};
        w.u32(r->id);
        w.u32(peer);
        broadcast(Channel::Reliable, message);
        applyAuthority(*r);
        return true;
    }
    pendingOwners_[entity] = peer;  // shared at the end of the step (spawned this step)
    return true;
}

void NetSession::applyAuthority(Replica& r) {
    auto& reg = scene_.registry();
    if (!reg.valid(r.entity)) return;
    const bool here = r.owner == localPeer() || role_ == NetRole::Offline;
    auto* identity = reg.try_get<NetworkIdentityComponent>(r.entity);
    auto* body = reg.try_get<RigidBodyComponent>(r.entity);
    PhysicsWorld* physics = hooks_.physics ? hooks_.physics() : nullptr;
    if (!here && body != nullptr && identity != nullptr && identity->syncTransform && body->motion == BodyMotion::Dynamic) {
        body->motion = BodyMotion::Kinematic;  // moved by what arrives, pushing what it touches
        r.kinematic = true;
        if (physics != nullptr) physics->refreshBody(r.entity);
    } else if (here && r.kinematic) {
        if (body != nullptr) body->motion = BodyMotion::Dynamic;
        r.kinematic = false;
        if (physics != nullptr) physics->refreshBody(r.entity);
    }
    if (here) r.samples.clear();
}

Pose NetSession::poseOf(entt::entity entity) const {
    const auto& t = scene_.registry().get<TransformComponent>(entity);
    return {t.position, t.rotationDeg, t.scale};
}

void NetSession::setPose(entt::entity entity, const Pose& pose) {
    auto& t = scene_.registry().get<TransformComponent>(entity);
    t.position = pose.position;
    t.rotationDeg = pose.rotation;
    t.scale = pose.scale;
}

void NetSession::resetReplication() {
    auto& reg = scene_.registry();
    PhysicsWorld* physics = hooks_.physics ? hooks_.physics() : nullptr;
    for (auto& [id, r] : replicas_) {
        if (!r.kinematic || !reg.valid(r.entity)) continue;
        if (auto* body = reg.try_get<RigidBodyComponent>(r.entity)) body->motion = BodyMotion::Dynamic;
        if (physics != nullptr) physics->refreshBody(r.entity);
    }
    replicas_.clear();
    ids_.clear();
    localOnly_.clear();
    unbound_.clear();
    pendingOwners_.clear();
    clocks_.clear();
    outgoing_.clear();
    nextId_ = 1;
    sendTimer_ = 0.0;
}

namespace {

/// The entity or ancestor a spawn or clone made (entt::null for the scene file's entities).
entt::entity spawnRootOf(const Scene& scene, entt::entity e) {
    for (entt::entity x = e; x != entt::null; x = scene.parentOf(x)) {
        if (scene.registry().all_of<SpawnOrigin>(x)) return x;
    }
    return entt::null;
}

/// Descendants in depth-first order (the order both sides make them in).
void descendants(const Scene& scene, entt::entity e, std::vector<entt::entity>& out) {
    for (entt::entity child : scene.childrenOf(e)) {
        out.push_back(child);
        descendants(scene, child, out);
    }
}

} // namespace

void NetSession::registerSceneEntities() {
    std::vector<entt::entity> found;
    for (entt::entity e : scene_.registry().view<NetworkIdentityComponent>()) {
        if (spawnRootOf(scene_, e) == entt::null) found.push_back(e);
    }
    std::ranges::sort(found, [&](entt::entity a, entt::entity b) { return scene_.uuidOf(a) < scene_.uuidOf(b); });
    for (entt::entity e : found) add(e, nextId_++, kHostPeer, false);
}

void NetSession::sceneLoaded(const std::string& path) {
    scenePath_ = path;
    resetReplication();
    if (role_ != NetRole::Host) return;
    ++epoch_;
    registerSceneEntities();
    for (PeerId client : clients_) sendWorld(client);
}

void NetSession::discoverEntities() {
    auto& reg = scene_.registry();
    // Gone on the host: gone everywhere.
    std::vector<std::uint32_t> gone;
    for (const auto& [id, r] : replicas_) {
        if (!reg.valid(r.entity)) gone.push_back(id);
    }
    std::ranges::sort(gone);
    for (std::uint32_t id : gone) {
        auto message = start(Msg::Despawn, epoch_);
        ByteWriter w{message};
        w.u32(id);
        broadcast(Channel::Reliable, message);
        ids_.erase(replicas_[id].entity);
        replicas_.erase(id);
    }
    std::erase_if(pendingOwners_, [&](const auto& entry) { return !reg.valid(entry.first); });
    std::erase_if(localOnly_, [&](entt::entity e) { return !reg.valid(e); });
    // New: the trees spawned or cloned this step.
    std::vector<entt::entity> fresh;
    for (entt::entity e : reg.view<NetworkIdentityComponent>()) {
        if (!ids_.contains(e) && !localOnly_.contains(e)) fresh.push_back(e);
    }
    for (entt::entity e : fresh) {
        if (ids_.contains(e)) continue;  // shared with its tree just now
        const entt::entity root = spawnRootOf(scene_, e);
        if (root == entt::null || ids_.contains(root)) {
            log::warn("Net", "'{}' has a Network Identity but was not made by spawn() or clone() - it stays on the host",
                      scene_.nameOf(e));
            localOnly_.insert(e);
            continue;
        }
        auto ownerOf = [&](entt::entity x) {
            const auto it = pendingOwners_.find(x);
            if (it == pendingOwners_.end()) return kHostPeer;
            const PeerId p = it->second;
            pendingOwners_.erase(it);
            return p;
        };
        Replica& r = add(root, nextId_++, ownerOf(root), true);
        const std::uint32_t rootId = r.id;
        for (PeerId client : clients_) sendSpawn(r, client);
        std::vector<entt::entity> below;
        descendants(scene_, root, below);
        std::vector<std::uint32_t> bound{rootId};
        for (entt::entity d : below) {
            if (!reg.all_of<NetworkIdentityComponent>(d) || ids_.contains(d)) continue;
            bound.push_back(add(d, nextId_++, ownerOf(d), false).id);
        }
        std::vector<const Replica*> binds;
        for (std::size_t i = 1; i < bound.size(); ++i) binds.push_back(&replicas_[bound[i]]);
        for (PeerId client : clients_) {
            for (auto& message : bindMessages(binds)) send(client, Channel::Reliable, message);
        }
        for (std::uint32_t id : bound) {
            Replica& b = replicas_[id];
            applyAuthority(b);
            sendValues(b, ~PeerId{0});
        }
    }
}

void NetSession::sendSpawn(const Replica& root, PeerId to) {
    const auto* origin = scene_.registry().try_get<SpawnOrigin>(root.entity);
    if (origin == nullptr) return;
    auto message = start(Msg::Spawn, epoch_);
    ByteWriter w{message};
    w.u32(root.id);
    w.u32(root.owner);
    const bool prefab = !origin->prefab.empty();
    w.u8(prefab ? kSpawnPrefab : kSpawnClone);
    if (prefab) {
        w.str(origin->prefab);
    } else {
        w.u64(origin->source.value());
    }
    w.u64(scene_.uuidOf(root.entity).value());
    writePose(w, poseOf(root.entity));
    std::vector<entt::entity> below;
    descendants(scene_, root.entity, below);
    const std::size_t room = (kMaxMessage - message.size() - 2) / 8;
    if (below.size() > room) {
        log::warn("Net", "'{}' has {} entities below it; only the first {} can be shared", scene_.nameOf(root.entity),
                  below.size(), room);
        below.resize(room);
    }
    w.u16(static_cast<std::uint16_t>(below.size()));
    for (entt::entity d : below) w.u64(scene_.uuidOf(d).value());
    send(to, Channel::Reliable, message);
}

std::vector<std::vector<std::byte>> NetSession::bindMessages(const std::vector<const Replica*>& replicas) const {
    std::vector<std::vector<std::byte>> out;
    const std::size_t perMessage = (kMaxMessage - kMessageHeader - 2) / kBindEntry;
    for (std::size_t first = 0; first < replicas.size(); first += perMessage) {
        const std::size_t count = std::min(perMessage, replicas.size() - first);
        auto message = start(Msg::Bind, epoch_);
        ByteWriter w{message};
        w.u16(static_cast<std::uint16_t>(count));
        for (std::size_t i = first; i < first + count; ++i) {
            const Replica& r = *replicas[i];
            w.u32(r.id);
            w.u64(scene_.uuidOf(r.entity).value());
            w.u32(r.owner);
            writePose(w, poseOf(r.entity));
        }
        out.push_back(std::move(message));
    }
    return out;
}

void NetSession::sendWorld(PeerId peer) {
    auto& reg = scene_.registry();
    auto scene = start(Msg::Scene, epoch_);
    ByteWriter(scene).str(scenePath_);
    send(peer, Channel::Reliable, scene);
    // Scene entities, then what was spawned (in order: a clone's source comes before it), each
    // followed by the shared entities inside it.
    std::vector<const Replica*> ordered;
    for (const auto& [id, r] : replicas_) {
        if (reg.valid(r.entity)) ordered.push_back(&r);
    }
    std::ranges::sort(ordered, {}, &Replica::id);
    std::vector<const Replica*> sceneOnes;
    for (const Replica* r : ordered) {
        if (!r->spawned && spawnRootOf(scene_, r->entity) == entt::null) sceneOnes.push_back(r);
    }
    for (auto& message : bindMessages(sceneOnes)) send(peer, Channel::Reliable, message);
    for (const Replica* r : ordered) {
        if (!r->spawned) continue;
        sendSpawn(*r, peer);
        std::vector<const Replica*> inside;
        std::vector<entt::entity> below;
        descendants(scene_, r->entity, below);
        for (entt::entity d : below) {
            if (const Replica* b = replicaOf(d)) inside.push_back(b);
        }
        for (auto& message : bindMessages(inside)) send(peer, Channel::Reliable, message);
    }
    send(peer, Channel::Reliable, start(Msg::BindDone, epoch_));
    for (const Replica* r : ordered) sendValues(*r, peer);
    for (const auto& [key, value] : globals_) sendValue(0, key, value, peer);
}

void NetSession::sendPeers() {
    auto message = start(Msg::Peers, epoch_);
    ByteWriter w{message};
    w.u16(static_cast<std::uint16_t>(clients_.size() + 1));
    w.u32(kHostPeer);
    for (PeerId c : clients_) w.u32(c);
    broadcast(Channel::Reliable, message);
}

void NetSession::sendValues(const Replica& replica, PeerId to) {
    const auto* values = scene_.registry().try_get<NetValuesComponent>(replica.entity);
    if (values == nullptr) return;
    for (const auto& [key, value] : values->values) sendValue(replica.id, key, value, to);
}

void NetSession::sendValue(std::uint32_t id, const std::string& key, const NetValue& value, PeerId to, PeerId except) {
    auto message = start(Msg::Value, epoch_);
    ByteWriter w{message};
    w.u32(id);
    w.str(key);
    writeValue(w, value);
    if (role_ == NetRole::Client) {
        send(kHostPeer, Channel::Reliable, message);
    } else if (to == ~PeerId{0}) {
        broadcast(Channel::Reliable, message, except);
    } else {
        send(to, Channel::Reliable, message);
    }
}

void NetSession::send(PeerId peer, Channel channel, const std::vector<std::byte>& message) {
    if (transport_) transport_->send(peer, channel, message);
}

void NetSession::broadcast(Channel channel, const std::vector<std::byte>& message, PeerId except) {
    for (PeerId c : clients_) {
        if (c != except) send(c, channel, message);
    }
}

// ---- Calls and values ------------------------------------------------------------------------

bool NetSession::rpc(entt::entity entity, std::string name, std::string data, RpcTarget target) {
    if (name.empty() || name.size() > 200 || data.size() + name.size() + 32 > kMaxMessage) {
        log::warn("Net", "rpc '{}': the name or data is too long ({} bytes of data at most)", name.substr(0, 40),
                  kMaxMessage - 32 - std::min<std::size_t>(name.size(), 200));
        return false;
    }
    if (role_ == NetRole::Connecting) return false;
    if (role_ == NetRole::Offline) {
        if (target == RpcTarget::Others) return false;  // nobody else plays
        localEvents_.push_back({NetScriptEvent::Kind::Rpc, entity, kHostPeer, std::move(name), std::move(data)});
        return true;
    }
    outgoing_.push_back({entity, std::move(name), std::move(data), target});
    return true;
}

void NetSession::sendRpcs() {
    auto calls = std::exchange(outgoing_, {});
    for (OutgoingRpc& call : calls) {
        std::uint32_t id = 0;
        if (call.entity != entt::null) {
            const Replica* r = replicaOf(call.entity);
            if (r == nullptr) {
                // Not shared: it exists only here.
                if (call.target != RpcTarget::Others) {
                    localEvents_.push_back({NetScriptEvent::Kind::Rpc, call.entity, localPeer(), call.name, call.data});
                }
                continue;
            }
            id = r->id;
        }
        if (role_ == NetRole::Host) {
            routeRpc(kHostPeer, id, call.name, call.data, call.target);
        } else {
            auto message = start(Msg::Rpc, epoch_);
            ByteWriter w{message};
            w.u32(id);
            w.u8(static_cast<std::uint8_t>(call.target));
            w.u32(localPeer());
            w.str(call.name);
            w.str(call.data);
            send(kHostPeer, Channel::Reliable, message);
        }
    }
}

void NetSession::routeRpc(PeerId sender, std::uint32_t id, const std::string& name, const std::string& data, RpcTarget target) {
    entt::entity entity = entt::null;
    PeerId owner = kHostPeer;
    if (id != 0) {
        const Replica* r = replica(id);
        if (r == nullptr) return;
        entity = r->entity;
        owner = r->owner;
    }
    auto message = start(Msg::Rpc, epoch_);
    ByteWriter w{message};
    w.u32(id);
    w.u8(static_cast<std::uint8_t>(target));
    w.u32(sender);
    w.str(name);
    w.str(data);
    bool here = false;
    switch (target) {
        case RpcTarget::Host: here = true; break;
        case RpcTarget::All:
            here = true;
            broadcast(Channel::Reliable, message);
            break;
        case RpcTarget::Others:
            here = sender != kHostPeer;
            broadcast(Channel::Reliable, message, sender);
            break;
        case RpcTarget::Owner:
            if (owner == kHostPeer) {
                here = true;
            } else {
                send(owner, Channel::Reliable, message);
            }
            break;
    }
    if (here) localEvents_.push_back({NetScriptEvent::Kind::Rpc, entity, sender, name, data});
}

bool NetSession::setValue(entt::entity entity, std::string key, NetValue value) {
    if (key.empty() || key.size() > 200 || (value.isText && value.text.size() > 900)) {
        log::warn("Net", "shared value '{}': the key or the text is too long", key.substr(0, 40));
        return false;
    }
    if (entity == entt::null) {
        if (!isServer()) return false;  // the game's values belong to the host
        auto [it, inserted] = globals_.try_emplace(key, value);
        if (!inserted && it->second == value) return true;
        it->second = value;
        if (role_ == NetRole::Host) sendValue(0, key, value, ~PeerId{0});
        return true;
    }
    if (!scene_.isValid(entity) || !hasAuthority(entity)) return false;
    auto& values = scene_.registry().get_or_emplace<NetValuesComponent>(entity).values;
    auto [it, inserted] = values.try_emplace(key, value);
    if (!inserted && it->second == value) return true;  // unchanged: nothing to send
    it->second = value;
    if (const Replica* r = replicaOf(entity); r != nullptr && role_ != NetRole::Offline) {
        sendValue(r->id, key, value, ~PeerId{0});
    }
    return true;
}

const NetValue* NetSession::value(entt::entity entity, std::string_view key) const {
    if (entity == entt::null) {
        const auto it = globals_.find(key);
        return it == globals_.end() ? nullptr : &it->second;
    }
    if (!scene_.isValid(entity)) return nullptr;
    const auto* values = scene_.registry().try_get<NetValuesComponent>(entity);
    if (values == nullptr) return nullptr;
    const auto it = values->values.find(key);
    return it == values->values.end() ? nullptr : &it->second;
}

// ---- Steps --------------------------------------------------------------------------------------

void NetSession::deliver(NetScriptEvent event) {
    if (event.entity != entt::null && !scene_.isValid(event.entity)) return;
    if (hooks_.deliver) hooks_.deliver(event);
}

void NetSession::beginStep(double /*dt*/, double simTime) {
    std::vector<NetEvent> events = std::exchange(pending_, {});
    if (transport_) {
        for (NetEvent& e : transport_->poll(now())) events.push_back(std::move(e));
    }
    auto queued = std::exchange(localEvents_, {});
    for (const NetEvent& e : events) {
        if (!transport_) break;  // the game ended on an earlier event
        handle(e, simTime);
    }
    // Behaviours hear about it after everything that arrived was applied (they may start or
    // end games themselves).
    auto arrived = std::exchange(localEvents_, {});
    for (NetScriptEvent& e : queued) deliver(std::move(e));
    for (NetScriptEvent& e : arrived) deliver(std::move(e));
}

void NetSession::endStep(double dt, double simTime) {
    auto& reg = scene_.registry();
    if (role_ == NetRole::Host) {
        discoverEntities();
        sendRpcs();
    } else if (role_ == NetRole::Client) {
        for (auto it = replicas_.begin(); it != replicas_.end();) {
            if (!reg.valid(it->second.entity)) {  // destroyed here (the host still has it)
                ids_.erase(it->second.entity);
                it = replicas_.erase(it);
            } else {
                ++it;
            }
        }
        sendRpcs();
    }
    if (role_ == NetRole::Host || role_ == NetRole::Client) {
        sendTimer_ += dt;
        if (sendTimer_ + 1e-9 >= sendInterval_) {
            sendTimer_ = std::fmod(sendTimer_, sendInterval_);
            sendStates(simTime);
        }
        smooth(dt);
    }
    if (transport_) {
        for (NetEvent& e : transport_->poll(now())) pending_.push_back(std::move(e));
    }
}

void NetSession::sendStates(double simTime) {
    auto& reg = scene_.registry();
    struct Entry {
        const Replica* replica;
        Pose pose;
    };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> order;
    for (const auto& [id, r] : replicas_) order.push_back(id);
    std::ranges::sort(order);
    for (std::uint32_t id : order) {
        Replica& r = replicas_[id];
        if (!reg.valid(r.entity)) continue;
        const auto* identity = reg.try_get<NetworkIdentityComponent>(r.entity);
        if (identity == nullptr || !identity->syncTransform) continue;
        if (role_ == NetRole::Client && r.owner != localPeer()) continue;
        const Pose pose = poseOf(r.entity);
        if (!finite(pose)) continue;
        if (r.sentAt >= 0.0 && same(pose, r.sent) && simTime - r.sentAt < kRefresh) continue;
        r.sent = pose;
        r.sentAt = simTime;
        entries.push_back({&r, pose});
    }
    if (entries.empty()) return;
    const std::size_t perMessage = (kMaxMessage - kMessageHeader - 8 - 2) / kStateEntry;
    auto write = [&](PeerId to, const std::vector<const Entry*>& list) {
        for (std::size_t first = 0; first < list.size(); first += perMessage) {
            const std::size_t count = std::min(perMessage, list.size() - first);
            auto message = start(role_ == NetRole::Host ? Msg::Snapshot : Msg::OwnerState, epoch_);
            ByteWriter w{message};
            w.f64(simTime);
            w.u16(static_cast<std::uint16_t>(count));
            for (std::size_t i = first; i < first + count; ++i) {
                w.u32(list[i]->replica->id);
                const Pose& p = list[i]->pose;
                const bool scaled = p.scale != glm::vec3(1.0f);
                w.u8(scaled ? 1 : 0);
                for (const glm::vec3& v : {p.position, p.rotation}) {
                    w.f32(v.x);
                    w.f32(v.y);
                    w.f32(v.z);
                }
                if (scaled) {
                    w.f32(p.scale.x);
                    w.f32(p.scale.y);
                    w.f32(p.scale.z);
                }
            }
            send(to, Channel::Unreliable, message);
        }
    };
    if (role_ == NetRole::Client) {
        std::vector<const Entry*> all;
        for (const Entry& e : entries) all.push_back(&e);
        write(kHostPeer, all);
        return;
    }
    for (PeerId client : clients_) {
        std::vector<const Entry*> list;
        for (const Entry& e : entries) {
            if (e.replica->owner != client) list.push_back(&e);  // a client moves its own
        }
        write(client, list);
    }
}

void NetSession::pushSample(Replica& r, PeerId source, double time, const Pose& pose) {
    if (!finite(pose) || !std::isfinite(time)) return;
    Clock& clock = clocks_[source];
    if (!clock.started) {
        clock.started = true;
        clock.latest = time;
        clock.play = time - delay_;
    }
    clock.latest = std::max(clock.latest, time);
    auto& s = r.samples;
    if (!s.empty() && time <= s.back().time) {
        // Late (reordered): put it in its place unless it is a duplicate.
        const auto at = std::ranges::find_if(s, [&](const Sample& x) { return x.time >= time; });
        if (at != s.end() && at->time == time) return;
        s.insert(at, {time, pose});
        return;
    }
    // It stood still until just before (unchanged entities are not sent): hold the old pose
    // until then instead of drifting across the whole pause.
    if (!s.empty() && time - s.back().time > sendInterval_ * 2.5) s.push_back({time - sendInterval_, s.back().pose});
    s.push_back({time, pose});
    while (s.size() > 64) s.pop_front();
}

void NetSession::smooth(double dt) {
    for (auto& [peer, clock] : clocks_) {
        if (!clock.started) continue;
        clock.play += dt;
        const double target = clock.latest - delay_;
        if (std::abs(clock.play - target) > 0.5) {
            clock.play = target;  // far off (a hitch, a new scene): jump
        } else {
            clock.play += (target - clock.play) * 0.1;
        }
    }
    auto& reg = scene_.registry();
    for (auto& [id, r] : replicas_) {
        if (r.samples.empty() || r.owner == localPeer() || !reg.valid(r.entity)) continue;
        const auto* identity = reg.try_get<NetworkIdentityComponent>(r.entity);
        if (identity == nullptr || !identity->syncTransform) continue;
        auto& s = r.samples;
        if (!identity->interpolate) {
            setPose(r.entity, s.back().pose);
            continue;
        }
        const PeerId source = role_ == NetRole::Client ? kHostPeer : r.owner;
        const double t = clocks_[source].play;
        while (s.size() > 2 && s[1].time <= t) s.pop_front();
        if (t <= s.front().time) {
            setPose(r.entity, s.front().pose);
        } else if (t >= s.back().time) {
            setPose(r.entity, s.back().pose);
        } else {
            const Sample& a = s[0];
            const Sample& b = s[1];
            const double span = b.time - a.time;
            const auto f = static_cast<float>(span > 1e-9 ? (t - a.time) / span : 1.0);
            setPose(r.entity, mix(a.pose, b.pose, std::clamp(f, 0.0f, 1.0f)));
        }
    }
}

// ---- What arrives -------------------------------------------------------------------------------

void NetSession::handle(const NetEvent& event, double simTime) {
    switch (event.kind) {
        case NetEvent::Kind::Connected:
            if (role_ == NetRole::Host) {
                clients_.push_back(event.peer);
                sendWorld(event.peer);
                sendPeers();
                localEvents_.push_back({NetScriptEvent::Kind::PeerJoined, entt::null, event.peer, {}, {}});
                log::info("Net", "player {} joined", event.peer);
            } else if (role_ == NetRole::Connecting) {
                role_ = NetRole::Client;
                localEvents_.push_back({NetScriptEvent::Kind::Connected, entt::null, kHostPeer, {}, {}});
                log::info("Net", "joined the game as player {}", localPeer());
            }
            break;
        case NetEvent::Kind::Disconnected:
            if (role_ == NetRole::Host) {
                std::erase(clients_, event.peer);
                clocks_.erase(event.peer);
                std::erase_if(pendingOwners_, [&](const auto& entry) { return entry.second == event.peer; });
                for (auto& [id, r] : replicas_) {
                    if (r.owner != event.peer) continue;
                    r.owner = kHostPeer;  // back to the host (behaviours may destroy it in onPeerLeft)
                    r.sentAt = -1.0;
                    auto message = start(Msg::Owner, epoch_);
                    ByteWriter w{message};
                    w.u32(id);
                    w.u32(kHostPeer);
                    broadcast(Channel::Reliable, message);
                    applyAuthority(r);
                }
                sendPeers();
                localEvents_.push_back({NetScriptEvent::Kind::PeerLeft, entt::null, event.peer, {}, event.reason});
                log::info("Net", "player {} left ({})", event.peer, event.reason);
            } else {
                becomeOffline(event.reason, true);
            }
            break;
        case NetEvent::Kind::Message: handleMessage(event.peer, event.data, simTime); break;
    }
}

entt::entity NetSession::makeSpawn(std::uint8_t kind, const std::string& prefab, Uuid source) {
    entt::entity made = entt::null;
    if (kind == kSpawnPrefab) {
        const std::filesystem::path relative = std::filesystem::path(prefab).lexically_normal();
        if (projectRoot_.empty() || relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
            log::warn("Net", "the host spawned '{}', which is not a prefab of this project", prefab);
            return entt::null;
        }
        auto& cached = prefabs_[prefab];
        if (!cached) {
            auto loaded = loadPrefab(projectRoot_ / relative);
            if (!loaded) {
                log::warn("Net", "the host spawned {}: {}", prefab, loaded.error().message);
                return entt::null;
            }
            cached = std::make_shared<const nlohmann::json>(std::move(*loaded));
        }
        auto root = instantiatePrefab(scene_, *cached);
        if (!root) {
            log::warn("Net", "the host spawned {}: {}", prefab, root.error().message);
            return entt::null;
        }
        made = *root;
    } else {
        const entt::entity original = scene_.findByUuid(source);
        if (original == entt::null) {
            log::warn("Net", "the host cloned an entity this game does not have");
            return entt::null;
        }
        made = scene_.duplicateEntity(original);
    }
    return made;
}

void NetSession::handleMessage(PeerId from, std::span<const std::byte> data, double /*simTime*/) {
    ByteReader r{data};
    const auto type = static_cast<Msg>(r.u8());
    const std::uint32_t epoch = r.u32();
    if (!r.ok) return;
    auto& reg = scene_.registry();
    auto readStates = [&](PeerId source, bool fromOwner) {
        const double time = r.f64();
        const std::uint16_t count = r.u16();
        for (std::uint16_t i = 0; i < count && r.ok; ++i) {
            const std::uint32_t id = r.u32();
            const std::uint8_t flags = r.u8();
            Pose pose;
            pose.position = {r.f32(), r.f32(), r.f32()};
            pose.rotation = {r.f32(), r.f32(), r.f32()};
            if ((flags & 1) != 0) pose.scale = {r.f32(), r.f32(), r.f32()};
            if (!r.ok) break;
            Replica* rep = replica(id);
            if (rep == nullptr || rep->owner == localPeer()) continue;
            if (fromOwner && rep->owner != source) continue;  // only the owner moves it
            pushSample(*rep, source, time, pose);
        }
    };

    if (role_ == NetRole::Host) {
        if (epoch != epoch_) return;  // from before the last scene change
        switch (type) {
            case Msg::OwnerState: readStates(from, true); break;
            case Msg::Rpc: {
                const std::uint32_t id = r.u32();
                const auto target = static_cast<RpcTarget>(r.u8());
                r.u32();  // (the sender is who sent it)
                const std::string name = r.str();
                const std::string body = r.str();
                if (!r.ok || name.empty() || static_cast<std::uint8_t>(target) > 3) return;
                if (id != 0 && replica(id) == nullptr) return;
                routeRpc(from, id, name, body, target);
                break;
            }
            case Msg::Value: {
                const std::uint32_t id = r.u32();
                const std::string key = r.str();
                const NetValue value = readValue(r);
                Replica* rep = id != 0 ? replica(id) : nullptr;
                if (!r.ok || rep == nullptr || rep->owner != from || !reg.valid(rep->entity)) return;
                reg.get_or_emplace<NetValuesComponent>(rep->entity).values[key] = value;
                sendValue(id, key, value, ~PeerId{0}, from);
                break;
            }
            default: break;  // clients send nothing else
        }
        return;
    }
    if (role_ != NetRole::Client || from != kHostPeer) return;

    switch (type) {
        case Msg::Scene: {
            const std::string path = r.str();
            if (!r.ok) return;
            epoch_ = epoch;
            if (!path.empty() && path != scenePath_) {
                if (!hooks_.loadScene || !hooks_.loadScene(path)) {  // (calls sceneLoaded)
                    becomeOffline(std::format("cannot load the host's scene {}", path), true);
                    return;
                }
            } else {
                resetReplication();
            }
            for (entt::entity e : reg.view<NetworkIdentityComponent>()) {
                if (spawnRootOf(scene_, e) == entt::null) unbound_.insert(e);
            }
            return;
        }
        case Msg::Peers: {
            const std::uint16_t count = r.u16();
            std::vector<PeerId> now;
            for (std::uint16_t i = 0; i < count && r.ok; ++i) {
                const PeerId p = r.u32();
                if (p != localPeer()) now.push_back(p);
            }
            if (!r.ok) return;
            for (PeerId p : now) {
                if (std::ranges::find(remotePeers_, p) == remotePeers_.end()) {
                    localEvents_.push_back({NetScriptEvent::Kind::PeerJoined, entt::null, p, {}, {}});
                }
            }
            for (PeerId p : remotePeers_) {
                if (std::ranges::find(now, p) == now.end()) {
                    localEvents_.push_back({NetScriptEvent::Kind::PeerLeft, entt::null, p, {}, {}});
                }
            }
            remotePeers_ = std::move(now);
            return;
        }
        default: break;
    }
    if (epoch != epoch_) return;

    switch (type) {
        case Msg::Bind: {
            const std::uint16_t count = r.u16();
            for (std::uint16_t i = 0; i < count && r.ok; ++i) {
                const std::uint32_t id = r.u32();
                const Uuid uuid(r.u64());
                const PeerId owner = r.u32();
                const Pose pose = readPose(r);
                if (!r.ok) break;
                const entt::entity e = scene_.findByUuid(uuid);
                if (e == entt::null) continue;
                Replica& rep = add(e, id, owner, false);
                if (finite(pose)) setPose(e, pose);
                unbound_.erase(e);
                applyAuthority(rep);
            }
            scene_.updateWorldTransforms();
            break;
        }
        case Msg::BindDone: {
            // What the host no longer has (destroyed before this player came).
            for (entt::entity e : unbound_) {
                if (reg.valid(e)) scene_.destroyEntity(e);
            }
            unbound_.clear();
            break;
        }
        case Msg::Spawn: {
            const std::uint32_t id = r.u32();
            const PeerId owner = r.u32();
            const std::uint8_t kind = r.u8();
            std::string prefab;
            Uuid source;
            if (kind == kSpawnPrefab) {
                prefab = r.str();
            } else {
                source = Uuid(r.u64());
            }
            const Uuid rootUuid(r.u64());
            const Pose pose = readPose(r);
            const std::uint16_t count = r.u16();
            std::vector<Uuid> below;
            for (std::uint16_t i = 0; i < count && r.ok; ++i) below.emplace_back(r.u64());
            if (!r.ok) return;
            const entt::entity e = makeSpawn(kind, prefab, source);
            if (e == entt::null) return;
            // The same UUIDs as on the host: later messages (and clones of it) find it.
            scene_.changeUuid(e, rootUuid);
            std::vector<entt::entity> made;
            descendants(scene_, e, made);
            for (std::size_t i = 0; i < made.size() && i < below.size(); ++i) scene_.changeUuid(made[i], below[i]);
            if (finite(pose)) setPose(e, pose);
            scene_.updateWorldTransforms();
            Replica& rep = add(e, id, owner, true);
            applyAuthority(rep);
            break;
        }
        case Msg::Despawn: {
            const std::uint32_t id = r.u32();
            if (Replica* rep = replica(id)) {
                if (reg.valid(rep->entity)) scene_.destroyEntity(rep->entity);
                ids_.erase(rep->entity);
                replicas_.erase(id);
            }
            break;
        }
        case Msg::Owner: {
            const std::uint32_t id = r.u32();
            const PeerId owner = r.u32();
            if (Replica* rep = replica(id); rep != nullptr && r.ok) {
                rep->owner = owner;
                rep->samples.clear();
                rep->sentAt = -1.0;
                applyAuthority(*rep);
            }
            break;
        }
        case Msg::Snapshot: readStates(kHostPeer, false); break;
        case Msg::Rpc: {
            const std::uint32_t id = r.u32();
            r.u8();  // target (the host chose who gets it)
            const PeerId sender = r.u32();
            const std::string name = r.str();
            std::string body = r.str();
            if (!r.ok) return;
            entt::entity entity = entt::null;
            if (id != 0) {
                const Replica* rep = replica(id);
                if (rep == nullptr) return;
                entity = rep->entity;
            }
            localEvents_.push_back({NetScriptEvent::Kind::Rpc, entity, sender, name, std::move(body)});
            break;
        }
        case Msg::Value: {
            const std::uint32_t id = r.u32();
            const std::string key = r.str();
            NetValue value = readValue(r);
            if (!r.ok) return;
            if (id == 0) {
                globals_[key] = std::move(value);
            } else if (const Replica* rep = replica(id); rep != nullptr && reg.valid(rep->entity)) {
                reg.get_or_emplace<NetValuesComponent>(rep->entity).values[key] = std::move(value);
            }
            break;
        }
        default: break;
    }
}

} // namespace lumora::net
