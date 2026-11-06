#include "scene/Particles.hpp"

#include "core/JobSystem.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lumora {

namespace {

/// xorshift32: fast, deterministic per emitter.
float random01(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

float vary(float value, float fraction, std::uint32_t& rng) {
    return value * (1.0f + fraction * (random01(rng) * 2.0f - 1.0f));
}

/// Direction in a cone of half-angle `spread` around +Y (uniform over the cap).
glm::vec3 coneDirection(float spreadRad, std::uint32_t& rng) {
    const float cosMax = std::cos(std::clamp(spreadRad, 0.0f, glm::pi<float>()));
    const float z = 1.0f - random01(rng) * (1.0f - cosMax);
    const float phi = random01(rng) * glm::two_pi<float>();
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(phi), z, r * std::sin(phi)};
}

glm::vec3 shapeOffset(const ParticleEmitterComponent& e, std::uint32_t& rng) {
    switch (e.shape) {
        case ParticleShape::Sphere: {
            // Uniform in the ball: a direction scaled by cbrt(u).
            const glm::vec3 d = coneDirection(glm::pi<float>(), rng);
            return d * (e.shapeSize * std::cbrt(random01(rng)));
        }
        case ParticleShape::Box:
            return glm::vec3(random01(rng) * 2.0f - 1.0f, random01(rng) * 2.0f - 1.0f, random01(rng) * 2.0f - 1.0f) *
                   e.shapeSize;
        case ParticleShape::Point:
        default:
            return glm::vec3(0.0f);
    }
}

void simulate(const ParticleEmitterComponent& e, ParticleStateComponent& s, const glm::mat4& world, float dt) {
    using Particle = ParticleStateComponent::Particle;
    // Age, move, drop the dead (swap-remove keeps it O(n)).
    const glm::vec3 gravity(0.0f, -9.81f * e.gravity, 0.0f);
    const float damping = std::exp(-std::max(e.drag, 0.0f) * dt);
    for (std::size_t i = 0; i < s.particles.size();) {
        Particle& p = s.particles[i];
        p.age += dt;
        if (p.age >= p.life) {
            p = s.particles.back();
            s.particles.pop_back();
            continue;
        }
        p.velocity = (p.velocity + gravity * dt) * damping;
        p.position += p.velocity * dt;
        ++i;
    }
    // Emit.
    int count = std::exchange(s.requested, 0);
    if (!s.started) {
        s.started = true;
        count += std::max(e.burst, 0);
    }
    if (e.playing && e.rate > 0.0f) {
        s.pending += e.rate * dt;
        const auto whole = static_cast<int>(s.pending);
        s.pending -= static_cast<float>(whole);
        count += whole;
    }
    const auto capacity = static_cast<int>(std::max(e.maxParticles, 0)) - static_cast<int>(s.particles.size());
    count = std::clamp(count, 0, std::max(capacity, 0));
    const glm::mat3 basis(world);
    const glm::vec3 origin(world[3]);
    for (int n = 0; n < count; ++n) {
        Particle p{};
        const glm::vec3 local = shapeOffset(e, s.rng);
        const glm::vec3 direction = coneDirection(glm::radians(e.spreadDeg), s.rng);
        const float speed = std::max(0.0f, vary(e.speed, e.speedVariation, s.rng));
        if (e.worldSpace) {
            p.position = origin + basis * local;
            const glm::vec3 axis = basis * direction;
            const float len = glm::length(axis);
            p.velocity = (len > 1e-6f ? axis / len : glm::vec3(0.0f, 1.0f, 0.0f)) * speed;
        } else {
            p.position = local;
            p.velocity = direction * speed;
        }
        p.life = std::max(0.01f, vary(e.lifetime, e.lifetimeVariation, s.rng));
        // Spread the batch over the step so a high rate makes a stream, not pulses.
        const float head = random01(s.rng) * dt;
        p.age = head;
        p.position += p.velocity * head;
        s.particles.push_back(p);
    }
}

} // namespace

void updateParticles(Scene& scene, float dt) {
    auto& reg = scene.registry();
    std::vector<entt::entity> emitters;
    for (auto e : reg.view<ParticleEmitterComponent>()) emitters.push_back(e);
    for (entt::entity e : emitters) {
        if (!reg.all_of<ParticleStateComponent>(e)) {
            auto& state = reg.emplace<ParticleStateComponent>(e);
            state.rng ^= static_cast<std::uint32_t>(entt::to_integral(e)) * 2654435761u;  // emitters differ
            if (state.rng == 0) state.rng = 1;
        }
    }
    // State of removed emitters goes too.
    std::vector<entt::entity> orphans;
    for (auto e : reg.view<ParticleStateComponent>(entt::exclude<ParticleEmitterComponent>)) orphans.push_back(e);
    for (entt::entity e : orphans) reg.remove<ParticleStateComponent>(e);
    if (emitters.empty() || dt <= 0.0f) return;
    JobSystem::global().parallelFor(emitters.size(), 4, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const entt::entity e = emitters[i];
            const auto* world = reg.try_get<WorldTransformComponent>(e);
            simulate(reg.get<ParticleEmitterComponent>(e), reg.get<ParticleStateComponent>(e),
                     world != nullptr ? world->matrix : glm::mat4(1.0f), dt);
        }
    });
}

std::size_t particleCount(const Scene& scene) {
    std::size_t n = 0;
    for (auto [e, state] : scene.registry().view<ParticleStateComponent>().each()) n += state.particles.size();
    return n;
}

void requestParticles(Scene& scene, unsigned entity, int count) {
    auto& reg = scene.registry();
    const auto e = static_cast<entt::entity>(entity);
    if (!reg.valid(e) || !reg.all_of<ParticleEmitterComponent>(e) || count <= 0) return;
    auto* state = reg.try_get<ParticleStateComponent>(e);
    if (state == nullptr) state = &reg.emplace<ParticleStateComponent>(e);
    state->requested += count;
}

} // namespace lumora
