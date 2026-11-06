#include "renderer/VirtualShadowMap.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <cmath>

namespace lumora::render {

namespace {

using Vec2 = glm::vec2;

float turn(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

/// Convex hull, counter-clockwise (monotone chain).
std::vector<Vec2> convexHull(std::vector<Vec2> points) {
    std::ranges::sort(points, [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (points.size() < 3) return points;
    std::vector<Vec2> hull(points.size() * 2);
    std::size_t k = 0;
    for (const Vec2& p : points) {
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], p) <= 0.0f) --k;
        hull[k++] = p;
    }
    for (std::size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && turn(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) --k;
        hull[k++] = points[i];
    }
    hull.resize(k - 1);
    return hull;
}

/// Separating axes: the rectangle's two and the hull's edge normals.
bool overlaps(Vec2 min, Vec2 max, const std::vector<Vec2>& hull) {
    Vec2 hmin(1e30f), hmax(-1e30f);
    for (const Vec2& p : hull) {
        hmin = glm::min(hmin, p);
        hmax = glm::max(hmax, p);
    }
    if (hmax.x < min.x || hmin.x > max.x || hmax.y < min.y || hmin.y > max.y) return false;
    const Vec2 corners[4] = {min, {max.x, min.y}, max, {min.x, max.y}};
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const Vec2 a = hull[i];
        const Vec2 b = hull[(i + 1) % hull.size()];
        const Vec2 outward(b.y - a.y, a.x - b.x);  // right of a->b: outside a counter-clockwise hull
        bool allOutside = true;
        for (const Vec2& c : corners) allOutside = allOutside && glm::dot(outward, c - a) > 0.0f;
        if (allOutside) return false;
    }
    return true;
}

int floorDiv(float v, float size) { return static_cast<int>(std::floor(v / size)); }
int slotOf(int page) { return ((page % VirtualShadowMap::kPagesPerLevel) + VirtualShadowMap::kPagesPerLevel) % VirtualShadowMap::kPagesPerLevel; }

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))); }

} // namespace

std::uint64_t VirtualShadowMap::pageKey(int level, int x, int y) noexcept {
    return (static_cast<std::uint64_t>(level) << 60) | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) & 0x3fffffffu) << 30) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) & 0x3fffffffu);
}

float VirtualShadowMap::pageSize(int level) noexcept {
    return 2.0f * kFirstHalf * std::exp2(static_cast<float>(level)) / static_cast<float>(kPagesPerLevel);
}

void VirtualShadowMap::invalidateAll() {
    for (auto& [key, page] : resident_) page.valid = false;
}

void VirtualShadowMap::invalidateRect(glm::vec2 min, glm::vec2 max) {
    for (auto& [key, page] : resident_) {
        if (!page.valid) continue;
        const float size = pageSize(page.level);
        const Vec2 pmin(static_cast<float>(page.x) * size, static_cast<float>(page.y) * size);
        if (max.x < pmin.x || min.x > pmin.x + size || max.y < pmin.y || min.y > pmin.y + size) continue;
        page.valid = false;
    }
}

VsmFrame VirtualShadowMap::update(const CameraView& camera, const glm::vec3& lightTravel, float shadowDistance,
                                  std::span<const VsmCaster> casters, std::uint32_t budget) {
    ++frame_;
    VsmFrame out;
    if (free_.empty() && resident_.empty()) {
        for (std::uint32_t i = kPoolPages * kPoolPages; i-- > 0;) free_.push_back(i);
    }
    // Light space: a fixed basis for the sun's direction, depth anchored near the camera.
    const glm::vec3 dir = glm::normalize(lightTravel);
    if (!initialised_ || glm::dot(dir, lightDir_) < 0.999999f) {
        lightDir_ = dir;
        const glm::vec3 up0 = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        right_ = glm::normalize(glm::cross(up0, dir));
        up_ = glm::cross(dir, right_);
        invalidateAll();
        casters_.clear();
        out.invalidatedAll = true;
    }
    const float range = std::max(shadowDistance * 2.0f, 64.0f) + 150.0f;
    const float anchor = std::round(glm::dot(dir, camera.position) / 32.0f) * 32.0f;
    if (!initialised_ || anchor - range != depthMin_ || range != depthRange_) {
        depthMin_ = anchor - range;
        depthRange_ = range;
        invalidateAll();
        out.invalidatedAll = true;
    }
    initialised_ = true;
    out.lightView = glm::mat4(1.0f);
    out.lightView = glm::row(out.lightView, 0, glm::vec4(right_, 0.0f));
    out.lightView = glm::row(out.lightView, 1, glm::vec4(up_, 0.0f));
    out.lightView = glm::row(out.lightView, 2, glm::vec4(dir, 0.0f));
    out.depthMin = depthMin_;
    out.depthScale = 1.0f / (2.0f * range);
    auto lightXY = [&](const glm::vec3& p) { return Vec2(glm::dot(right_, p), glm::dot(up_, p)); };

    // Levels: level l serves view depths whose frustum slice still fits inside its 16 pages.
    const float tanHalf = std::tan(camera.fovYRadians * 0.5f);
    const float spread = std::sqrt(1.0f + tanHalf * tanHalf * (1.0f + camera.aspect * camera.aspect));
    out.levelReach = (1.0f - 1.0f / (kPagesPerLevel / 2.0f)) / spread;
    out.firstHalf = kFirstHalf;
    out.levels = 1;
    while (out.levels < kMaxLevels && kFirstHalf * std::exp2(static_cast<float>(out.levels - 1)) * out.levelReach < shadowDistance) {
        ++out.levels;
    }

    // Casters: what changed since the last frame invalidates the pages under it.
    std::unordered_map<std::uint64_t, Tracked> now;
    now.reserve(casters.size());
    for (const VsmCaster& c : casters) {
        const Vec2 center = lightXY(c.center);
        const Vec2 extent(glm::dot(glm::abs(right_), c.extent), glm::dot(glm::abs(up_), c.extent));
        auto [it, fresh] = now.try_emplace(c.key);
        Tracked& t = it->second;
        if (fresh) {
            t.min = center - extent;
            t.max = center + extent;
        } else {
            t.min = glm::min(t.min, center - extent);
            t.max = glm::max(t.max, center + extent);
        }
        t.state = mix(t.state, c.state);
        t.dynamic = t.dynamic || c.dynamic;
    }
    for (auto& [key, t] : now) {
        const auto old = casters_.find(key);
        if (old == casters_.end()) {
            invalidateRect(t.min, t.max);
        } else if (old->second.state != t.state || t.dynamic ||
                   glm::any(glm::notEqual(old->second.min, t.min)) || glm::any(glm::notEqual(old->second.max, t.max))) {
            invalidateRect(old->second.min, old->second.max);
            invalidateRect(t.min, t.max);
        }
    }
    for (const auto& [key, t] : casters_) {
        if (!now.contains(key)) invalidateRect(t.min, t.max);  // gone
    }
    casters_ = std::move(now);

    // Requests: per level, the pages under the view frustum up to that level's reach.
    const glm::mat4 world = glm::inverse(camera.view);
    const glm::vec3 eye(world[3]);
    const glm::vec3 camRight = glm::normalize(glm::vec3(world[0]));
    const glm::vec3 camUp = glm::normalize(glm::vec3(world[1]));
    const glm::vec3 camForward = -glm::normalize(glm::vec3(world[2]));
    struct Need {
        int level, x, y;
        float distance;
    };
    std::vector<Need> needs;
    const Vec2 eyeXY = lightXY(eye);
    for (int l = 0; l < out.levels; ++l) {
        const float size = pageSize(l);
        const float far = std::min(kFirstHalf * std::exp2(static_cast<float>(l)) * out.levelReach, shadowDistance);
        std::vector<Vec2> points;
        for (const float d : {std::max(camera.nearPlane, 0.01f), std::max(far, camera.nearPlane * 2.0f)}) {
            const float h = d * tanHalf;
            const float w = h * camera.aspect;
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) points.push_back(lightXY(eye + camForward * d + camRight * (w * sx) + camUp * (h * sy)));
            }
        }
        const std::vector<Vec2> hull = convexHull(points);
        const int cx = floorDiv(eyeXY.x, size);
        const int cy = floorDiv(eyeXY.y, size);
        const float margin = size * 0.05f;  // filter and normal offset near a page's edge
        for (int y = cy - kPagesPerLevel / 2; y < cy + kPagesPerLevel / 2; ++y) {
            for (int x = cx - kPagesPerLevel / 2; x < cx + kPagesPerLevel / 2; ++x) {
                const Vec2 min(static_cast<float>(x) * size - margin, static_cast<float>(y) * size - margin);
                const Vec2 max(static_cast<float>(x + 1) * size + margin, static_cast<float>(y + 1) * size + margin);
                if (!overlaps(min, max, hull)) continue;
                needs.push_back({l, x, y, glm::length((min + max) * 0.5f - eyeXY)});
            }
        }
    }
    // Coarse levels first: they are the fallback when the budget runs out.
    std::ranges::sort(needs, [](const Need& a, const Need& b) {
        return a.level != b.level ? a.level > b.level : a.distance < b.distance;
    });
    for (const Need& n : needs) {
        if (auto it = resident_.find(pageKey(n.level, n.x, n.y)); it != resident_.end()) it->second.lastUsed = frame_;
    }
    std::vector<std::uint64_t> evictable;
    bool evictableBuilt = false;
    std::size_t nextEviction = 0;
    for (const Need& n : needs) {
        const std::uint64_t key = pageKey(n.level, n.x, n.y);
        auto it = resident_.find(key);
        if (it == resident_.end()) {
            std::uint32_t physical = 0;
            if (!free_.empty()) {
                physical = free_.back();
                free_.pop_back();
            } else {
                if (!evictableBuilt) {  // least recently used pages the view does not need
                    for (const auto& [k, page] : resident_) {
                        if (page.lastUsed < frame_) evictable.push_back(k);
                    }
                    std::ranges::sort(evictable, [&](std::uint64_t a, std::uint64_t b) {
                        return resident_.at(a).lastUsed < resident_.at(b).lastUsed;
                    });
                    evictableBuilt = true;
                }
                if (nextEviction >= evictable.size()) continue;  // the pool is full of needed pages
                const auto victim = resident_.find(evictable[nextEviction++]);
                physical = victim->second.physical;
                resident_.erase(victim);
            }
            it = resident_.emplace(key, Resident{physical, n.level, n.x, n.y, false, frame_}).first;
        }
        ++out.requested;
        Resident& page = it->second;
        if (page.valid) {
            ++out.cached;
            continue;
        }
        if (budget != 0 && out.render.size() >= budget) continue;
        const float size = pageSize(n.level);
        VsmPageDraw draw;
        draw.physical = page.physical;
        draw.level = n.level;
        draw.rectMin = Vec2(static_cast<float>(n.x) * size, static_cast<float>(n.y) * size);
        draw.rectMax = draw.rectMin + size;
        draw.center = (draw.rectMin + draw.rectMax) * 0.5f;
        draw.invHalf = 2.0f / size;
        out.render.push_back(draw);
        page.valid = true;  // drawn before anything samples it this frame
    }

    // Page table: every valid page inside each level's window.
    out.pageTable.assign(static_cast<std::size_t>(kPagesPerLevel * kPagesPerLevel * out.levels) * 4, 0.0f);
    for (int l = 0; l < out.levels; ++l) {
        const float size = pageSize(l);
        const int cx = floorDiv(eyeXY.x, size);
        const int cy = floorDiv(eyeXY.y, size);
        for (int y = cy - kPagesPerLevel / 2; y < cy + kPagesPerLevel / 2; ++y) {
            for (int x = cx - kPagesPerLevel / 2; x < cx + kPagesPerLevel / 2; ++x) {
                const auto it = resident_.find(pageKey(l, x, y));
                if (it == resident_.end() || !it->second.valid) continue;
                const std::size_t texel = static_cast<std::size_t>((l * kPagesPerLevel + slotOf(y)) * kPagesPerLevel + slotOf(x));
                out.pageTable[texel * 4 + 0] = static_cast<float>(it->second.physical);
                out.pageTable[texel * 4 + 1] = static_cast<float>(x);
                out.pageTable[texel * 4 + 2] = static_cast<float>(y);
                out.pageTable[texel * 4 + 3] = 1.0f;
            }
        }
    }
    return out;
}

} // namespace lumora::render
