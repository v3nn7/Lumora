#include "runtime/Navigation.hpp"

#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "scene/Scene.hpp"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace lumora {

namespace {

constexpr int kMaxAgents = 256;
constexpr int kMaxPathPolys = 512;
constexpr unsigned short kWalkable = 1;
const float kExtents[3] = {2.0f, 4.0f, 2.0f};  // search box for points near the mesh

/// Triangles of the static colliders (world space): the ground the agents stand on.
void collectGeometry(const Scene& scene, std::vector<float>& vertices, std::vector<int>& triangles) {
    const auto& reg = scene.registry();
    auto vertex = [&](const glm::vec3& p) {
        vertices.insert(vertices.end(), {p.x, p.y, p.z});
        return static_cast<int>(vertices.size() / 3 - 1);
    };
    for (auto [e, collider] : reg.view<ColliderComponent>().each()) {
        if (collider.isTrigger) continue;
        if (const auto* rb = reg.try_get<RigidBodyComponent>(e); rb != nullptr && rb->motion != BodyMotion::Static) continue;
        const glm::mat4 world = scene.worldMatrix(e);
        auto at = [&](const glm::vec3& local) { return glm::vec3(world * glm::vec4(collider.offset + local, 1.0f)); };
        const glm::vec3 center = at(glm::vec3(0.0f));
        const std::size_t firstTriangle = triangles.size();
        switch (collider.shape) {
            case ColliderShape::Box: {
                const glm::vec3 h = collider.size * 0.5f;
                int v[8];
                for (int i = 0; i < 8; ++i) {
                    v[i] = vertex(at({(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z}));
                }
                const int faces[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
                for (const auto& f : faces) {
                    triangles.insert(triangles.end(), {v[f[0]], v[f[1]], v[f[2]], v[f[0]], v[f[2]], v[f[3]]});
                }
                break;
            }
            case ColliderShape::Sphere:
            case ColliderShape::Capsule: {
                // A coarse UV sphere (stretched into a capsule along local Y).
                const float r = collider.radius;
                const float half = collider.shape == ColliderShape::Capsule ? collider.height * 0.5f : 0.0f;
                constexpr int kRings = 6, kSegments = 10;
                const int first = static_cast<int>(vertices.size() / 3);
                for (int ring = 0; ring <= kRings; ++ring) {
                    const float phi = glm::pi<float>() * static_cast<float>(ring) / kRings;
                    const float y = std::cos(phi) * r + (ring <= kRings / 2 ? half : -half);
                    for (int s = 0; s < kSegments; ++s) {
                        const float theta = glm::two_pi<float>() * static_cast<float>(s) / kSegments;
                        vertex(at({std::sin(phi) * std::cos(theta) * r, y, std::sin(phi) * std::sin(theta) * r}));
                    }
                }
                for (int ring = 0; ring < kRings; ++ring) {
                    for (int s = 0; s < kSegments; ++s) {
                        const int a = first + ring * kSegments + s;
                        const int b = first + ring * kSegments + (s + 1) % kSegments;
                        const int c = a + kSegments;
                        const int d = b + kSegments;
                        triangles.insert(triangles.end(), {a, b, c, b, d, c});
                    }
                }
                break;
            }
        }
        // Recast reads the slope from each triangle's winding: make every one face outwards.
        auto point = [&](int i) { return glm::vec3(vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]); };
        for (std::size_t t = firstTriangle; t + 2 < triangles.size(); t += 3) {
            const glm::vec3 p0 = point(triangles[t]), p1 = point(triangles[t + 1]), p2 = point(triangles[t + 2]);
            const glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
            if (glm::dot(normal, (p0 + p1 + p2) / 3.0f - center) < 0.0f) std::swap(triangles[t + 1], triangles[t + 2]);
        }
    }
}

} // namespace

struct NavigationWorld::Impl {
    Scene& scene;
    NavMeshSettings settings;
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtCrowd* crowd = nullptr;
    dtQueryFilter filter;
    std::size_t polygons = 0;
    bool attempted = false;
    std::unordered_map<entt::entity, int> agents;  // entity -> crowd index
    std::unordered_map<entt::entity, glm::vec3> targets;

    Impl(Scene& s, NavMeshSettings set) : scene(s), settings(set) {
        filter.setIncludeFlags(0xffff);
        filter.setExcludeFlags(0);
    }
    ~Impl() { release(); }

    void release() {
        if (crowd != nullptr) dtFreeCrowd(crowd);
        if (query != nullptr) dtFreeNavMeshQuery(query);
        if (mesh != nullptr) dtFreeNavMesh(mesh);
        crowd = nullptr;
        query = nullptr;
        mesh = nullptr;
        agents.clear();
        targets.clear();
        polygons = 0;
    }

    dtPolyRef nearest(const glm::vec3& p, glm::vec3& onMesh) const {
        dtPolyRef ref = 0;
        float point[3];
        query->findNearestPoly(&p.x, kExtents, &filter, &ref, point);
        onMesh = {point[0], point[1], point[2]};
        return ref;
    }
};

NavigationWorld::NavigationWorld(Scene& scene, NavMeshSettings settings)
    : impl_(std::make_unique<Impl>(scene, settings)) {}

NavigationWorld::~NavigationWorld() = default;

bool NavigationWorld::sceneHasAgents(const Scene& scene) { return !scene.registry().view<NavAgentComponent>().empty(); }
bool NavigationWorld::built() const noexcept { return impl_->query != nullptr; }
std::size_t NavigationWorld::polygonCount() const { return impl_->polygons; }

Result<> NavigationWorld::build() {
    Impl& w = *impl_;
    w.release();
    w.attempted = true;
    Stopwatch timer;
    std::vector<float> verts;
    std::vector<int> tris;
    collectGeometry(w.scene, verts, tris);
    if (tris.empty()) return makeError("no static colliders to walk on");
    const NavMeshSettings& s = w.settings;

    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.maxSlopeDeg;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.maxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch;
    const int vertCount = static_cast<int>(verts.size() / 3);
    const int triCount = static_cast<int>(tris.size() / 3);
    rcCalcBounds(verts.data(), vertCount, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    if (static_cast<long long>(cfg.width) * cfg.height > 6000LL * 6000LL) {
        return makeError("the walkable area is too large for one navigation mesh ({} x {} cells)", cfg.width, cfg.height);
    }

    rcContext ctx(false);
    rcHeightfield* solid = rcAllocHeightfield();
    std::vector<unsigned char> areas(static_cast<std::size_t>(triCount), 0);
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* contours = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    auto cleanup = [&] {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(contours);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    };
    auto fail = [&](const char* what) -> Result<> {
        cleanup();
        return makeError("navigation mesh: {} failed", what);
    };
    if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return fail("heightfield");
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), vertCount, tris.data(), triCount, areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), vertCount, tris.data(), areas.data(), triCount, *solid, cfg.walkableClimb)) {
        return fail("rasterisation");
    }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
    chf = rcAllocCompactHeightfield();
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf)) return fail("compaction");
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) return fail("erosion");
    if (!rcBuildDistanceField(&ctx, *chf)) return fail("distance field");
    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return fail("regions");
    contours = rcAllocContourSet();
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours)) return fail("contours");
    pmesh = rcAllocPolyMesh();
    if (!rcBuildPolyMesh(&ctx, *contours, cfg.maxVertsPerPoly, *pmesh)) return fail("polygon mesh");
    dmesh = rcAllocPolyMeshDetail();
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
        return fail("detail mesh");
    }
    if (pmesh->npolys == 0) {
        cleanup();
        return makeError("navigation mesh: nothing walkable (is the ground a static collider?)");
    }
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->flags[i] = kWalkable;
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.maxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int dataSize = 0;
    const int polygons = pmesh->npolys;
    if (!dtCreateNavMeshData(&params, &data, &dataSize)) return fail("Detour data");
    cleanup();
    w.mesh = dtAllocNavMesh();
    if (dtStatusFailed(w.mesh->init(data, dataSize, DT_TILE_FREE_DATA))) {
        dtFree(data);
        w.release();
        return makeError("navigation mesh: Detour init failed");
    }
    w.query = dtAllocNavMeshQuery();
    w.crowd = dtAllocCrowd();
    if (dtStatusFailed(w.query->init(w.mesh, 4096)) || !w.crowd->init(kMaxAgents, std::max(s.agentRadius, 2.0f), w.mesh)) {
        w.release();
        return makeError("navigation mesh: query init failed");
    }
    w.polygons = static_cast<std::size_t>(polygons);
    log::info("Navigation", "navigation mesh: {} polygons from {} triangles in {:.1f} ms", polygons, triCount,
              timer.elapsedMs());
    return {};
}

std::optional<glm::vec3> NavigationWorld::nearestPoint(const glm::vec3& position) {
    if (!built() && !impl_->attempted) (void)build();
    if (!built()) return std::nullopt;
    glm::vec3 onMesh;
    if (impl_->nearest(position, onMesh) == 0) return std::nullopt;
    return onMesh;
}

std::vector<glm::vec3> NavigationWorld::findPath(const glm::vec3& from, const glm::vec3& to) {
    Impl& w = *impl_;
    if (!built() && !w.attempted) {
        if (auto b = build(); !b) log::warn("Navigation", "{}", b.error().message);
    }
    if (!built()) return {};
    glm::vec3 start, end;
    const dtPolyRef startRef = w.nearest(from, start);
    const dtPolyRef endRef = w.nearest(to, end);
    if (startRef == 0 || endRef == 0) return {};
    dtPolyRef polys[kMaxPathPolys];
    int polyCount = 0;
    if (dtStatusFailed(w.query->findPath(startRef, endRef, &start.x, &end.x, &w.filter, polys, &polyCount, kMaxPathPolys)) ||
        polyCount == 0 || polys[polyCount - 1] != endRef) {
        return {};  // no complete way there
    }
    float straight[kMaxPathPolys * 3];
    int count = 0;
    w.query->findStraightPath(&start.x, &end.x, polys, polyCount, straight, nullptr, nullptr, &count, kMaxPathPolys);
    std::vector<glm::vec3> path;
    for (int i = 0; i < count; ++i) path.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
    return path;
}

bool NavigationWorld::moveTo(entt::entity agent, const glm::vec3& target) {
    Impl& w = *impl_;
    if (!w.scene.registry().all_of<NavAgentComponent>(agent)) return false;
    if (findPath(w.scene.worldMatrix(agent)[3], target).empty()) return false;
    w.targets[agent] = target;
    if (auto it = w.agents.find(agent); it != w.agents.end()) {
        glm::vec3 onMesh;
        const dtPolyRef ref = w.nearest(target, onMesh);
        w.crowd->requestMoveTarget(it->second, ref, &onMesh.x);
    }
    return true;
}

void NavigationWorld::stop(entt::entity agent) {
    Impl& w = *impl_;
    w.targets.erase(agent);
    if (auto it = w.agents.find(agent); it != w.agents.end()) w.crowd->resetMoveTarget(it->second);
}

bool NavigationWorld::isMoving(entt::entity agent) const { return impl_->targets.contains(agent); }

void NavigationWorld::update(float dt, PhysicsWorld* physics) {
    Impl& w = *impl_;
    auto& reg = w.scene.registry();
    if (!sceneHasAgents(w.scene)) return;
    if (!built()) {
        if (w.attempted) return;
        if (auto b = build(); !b) {
            log::warn("Navigation", "{}", b.error().message);
            return;
        }
    }
    // Agents that left the scene leave the crowd; new ones join it on the mesh.
    for (auto it = w.agents.begin(); it != w.agents.end();) {
        if (!reg.valid(it->first) || !reg.all_of<NavAgentComponent>(it->first)) {
            w.crowd->removeAgent(it->second);
            w.targets.erase(it->first);
            it = w.agents.erase(it);
        } else {
            ++it;
        }
    }
    for (auto [e, nav] : reg.view<NavAgentComponent>().each()) {
        if (w.agents.contains(e)) continue;
        glm::vec3 onMesh;
        if (w.nearest(glm::vec3(w.scene.worldMatrix(e)[3]), onMesh) == 0) continue;
        dtCrowdAgentParams params{};
        params.radius = nav.radius;
        params.height = nav.height;
        params.maxAcceleration = nav.acceleration;
        params.maxSpeed = nav.speed;
        params.collisionQueryRange = nav.radius * 12.0f;
        params.pathOptimizationRange = nav.radius * 30.0f;
        params.separationWeight = 2.0f;
        params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO |
                             DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        const int index = w.crowd->addAgent(&onMesh.x, &params);
        if (index < 0) continue;
        w.agents[e] = index;
        if (auto target = w.targets.find(e); target != w.targets.end()) {
            glm::vec3 goal;
            const dtPolyRef ref = w.nearest(target->second, goal);
            w.crowd->requestMoveTarget(index, ref, &goal.x);
        }
    }
    // Speed edits in the Inspector apply live.
    for (const auto& [e, index] : w.agents) {
        const auto& nav = reg.get<NavAgentComponent>(e);
        dtCrowdAgentParams params = w.crowd->getAgent(index)->params;
        if (params.maxSpeed != nav.speed || params.maxAcceleration != nav.acceleration) {
            params.maxSpeed = nav.speed;
            params.maxAcceleration = nav.acceleration;
            w.crowd->updateAgentParameters(index, &params);
        }
    }
    w.crowd->update(dt, nullptr);
    for (const auto& [e, index] : w.agents) {
        const dtCrowdAgent* agent = w.crowd->getAgent(index);
        const glm::vec3 position(agent->npos[0], agent->npos[1], agent->npos[2]);
        const glm::vec3 velocity(agent->vel[0], agent->vel[1], agent->vel[2]);
        if (auto target = w.targets.find(e); target != w.targets.end()) {
            const glm::vec3 d = target->second - position;
            if (glm::length(glm::vec2(d.x, d.z)) < std::max(0.15f, reg.get<NavAgentComponent>(e).radius * 0.5f)) {
                w.crowd->resetMoveTarget(index);
                w.targets.erase(target);
            }
        }
        const bool character = reg.all_of<CharacterControllerComponent>(e) && physics != nullptr;
        if (character) {
            // The physics walks the character; the crowd agent follows where it really is.
            physics->moveCharacter(e, glm::vec3(velocity.x, 0.0f, velocity.z));
            const glm::vec3 actual(w.scene.worldMatrix(e)[3]);
            dtCrowdAgent* mutableAgent = w.crowd->getEditableAgent(index);
            glm::vec3 snapped;
            if (w.nearest(actual, snapped) != 0) std::memcpy(mutableAgent->npos, &snapped.x, sizeof(float) * 3);
        } else {
            glm::vec3 worldPos = position;
            const entt::entity parent = w.scene.parentOf(e);
            if (parent != entt::null) worldPos = glm::vec3(glm::inverse(w.scene.worldMatrix(parent)) * glm::vec4(worldPos, 1.0f));
            reg.get<TransformComponent>(e).position = worldPos;
            // Face the way it walks.
            if (glm::length(glm::vec2(velocity.x, velocity.z)) > 0.05f) {
                reg.get<TransformComponent>(e).rotationDeg.y = glm::degrees(std::atan2(velocity.x, velocity.z));
            }
        }
    }
}

} // namespace lumora
