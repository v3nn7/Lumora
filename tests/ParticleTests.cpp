#include "scene/Particles.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

using namespace lumora;

namespace {

entt::entity addEmitter(Scene& scene, const ParticleEmitterComponent& settings, glm::vec3 position = {}) {
    const entt::entity e = scene.createEntity("Emitter");
    scene.registry().emplace<ParticleEmitterComponent>(e, settings);
    scene.registry().get<TransformComponent>(e).position = position;
    scene.updateWorldTransforms();
    return e;
}

const std::vector<ParticleStateComponent::Particle>& particlesOf(const Scene& scene, entt::entity e) {
    return scene.registry().get<ParticleStateComponent>(e).particles;
}

void run(Scene& scene, float seconds, float dt = 1.0f / 60.0f) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += dt) updateParticles(scene, dt);
}

} // namespace

TEST(Particles, EmitAtTheirRateAndDieAfterTheirLifetime) {
    Scene scene;
    ParticleEmitterComponent p;
    p.rate = 30.0f;
    p.lifetime = 0.5f;
    p.lifetimeVariation = 0.0f;
    const entt::entity e = addEmitter(scene, p);
    run(scene, 0.25f);
    EXPECT_NEAR(static_cast<float>(particlesOf(scene, e).size()), 7.5f, 1.5f) << "30/s for a quarter second";
    run(scene, 2.0f);
    EXPECT_NEAR(static_cast<float>(particlesOf(scene, e).size()), 15.0f, 1.5f) << "steady state: rate x lifetime";
    scene.registry().get<ParticleEmitterComponent>(e).playing = false;
    run(scene, 0.6f);
    EXPECT_TRUE(particlesOf(scene, e).empty()) << "stopped: the last ones die out";
    EXPECT_EQ(particleCount(scene), 0u);
}

TEST(Particles, BurstRequestsAndTheCap) {
    Scene scene;
    ParticleEmitterComponent p;
    p.playing = false;
    p.burst = 40;
    p.maxParticles = 50;
    p.lifetime = 10.0f;
    const entt::entity e = addEmitter(scene, p);
    updateParticles(scene, 0.01f);
    EXPECT_EQ(particlesOf(scene, e).size(), 40u) << "the burst, once";
    updateParticles(scene, 0.01f);
    EXPECT_EQ(particlesOf(scene, e).size(), 40u);
    requestParticles(scene, static_cast<unsigned>(entt::to_integral(e)), 30);
    updateParticles(scene, 0.01f);
    EXPECT_EQ(particlesOf(scene, e).size(), 50u) << "capped at Max Particles";
}

TEST(Particles, LeaveAlongTheEntitysUpAxisAndFall) {
    Scene scene;
    ParticleEmitterComponent p;
    p.playing = false;
    p.burst = 20;
    p.speed = 4.0f;
    p.speedVariation = 0.0f;
    p.spreadDeg = 0.0f;
    p.lifetime = 10.0f;
    p.gravity = 1.0f;
    const entt::entity e = addEmitter(scene, p, {1.0f, 2.0f, 3.0f});
    scene.registry().get<TransformComponent>(e).rotationDeg = {0.0f, 0.0f, -90.0f};  // +Y turned to +X
    scene.updateWorldTransforms();
    updateParticles(scene, 1e-4f);
    for (const auto& particle : particlesOf(scene, e)) {
        EXPECT_NEAR(particle.velocity.x, 4.0f, 1e-2f);
        EXPECT_NEAR(particle.velocity.y, 0.0f, 1e-2f);
        EXPECT_NEAR(glm::length(particle.position - glm::vec3(1.0f, 2.0f, 3.0f)), 0.0f, 1e-2f) << "from the emitter";
    }
    run(scene, 1.0f);
    for (const auto& particle : particlesOf(scene, e)) {
        EXPECT_NEAR(particle.position.x, 5.0f, 0.1f) << "4 m/s for a second";
        EXPECT_NEAR(particle.position.y, 2.0f - 0.5f * 9.81f, 0.2f) << "fell under gravity";
    }
}

TEST(Particles, ShapesAndSpreadStayInBounds) {
    Scene scene;
    ParticleEmitterComponent p;
    p.playing = false;
    p.burst = 500;
    p.maxParticles = 500;
    p.shape = ParticleShape::Sphere;
    p.shapeSize = 0.5f;
    p.spreadDeg = 30.0f;
    p.speed = 1.0f;
    p.speedVariation = 0.0f;
    p.lifetime = 10.0f;
    const entt::entity e = addEmitter(scene, p);
    updateParticles(scene, 1e-5f);
    ASSERT_EQ(particlesOf(scene, e).size(), 500u);
    float minCos = 1.0f;
    for (const auto& particle : particlesOf(scene, e)) {
        EXPECT_LE(glm::length(particle.position), 0.5f + 1e-3f);
        minCos = std::min(minCos, particle.velocity.y / glm::length(particle.velocity));
    }
    EXPECT_GE(minCos, std::cos(glm::radians(30.0f)) - 1e-3f) << "inside the cone";
    EXPECT_LT(minCos, std::cos(glm::radians(20.0f))) << "and filling it";
}

TEST(Particles, LocalSpaceParticlesAreRelativeToTheEmitter) {
    Scene scene;
    ParticleEmitterComponent p;
    p.playing = false;
    p.burst = 1;
    p.speed = 0.0f;
    p.lifetime = 10.0f;
    p.worldSpace = false;
    const entt::entity e = addEmitter(scene, p, {5.0f, 0.0f, 0.0f});
    updateParticles(scene, 0.01f);
    ASSERT_EQ(particlesOf(scene, e).size(), 1u);
    EXPECT_NEAR(glm::length(particlesOf(scene, e)[0].position), 0.0f, 1e-4f) << "stored relative to the emitter";
}

TEST(Particles, StateFollowsTheComponent) {
    Scene scene;
    ParticleEmitterComponent p;
    p.burst = 5;
    const entt::entity e = addEmitter(scene, p);
    updateParticles(scene, 0.01f);
    EXPECT_TRUE(scene.registry().all_of<ParticleStateComponent>(e));
    scene.registry().remove<ParticleEmitterComponent>(e);
    updateParticles(scene, 0.01f);
    EXPECT_FALSE(scene.registry().all_of<ParticleStateComponent>(e)) << "removed with the emitter";
}
