#pragma once

#include <cstddef>

namespace lumora {

class Scene;

/// Advances every Particle Emitter of a scene by dt: emits (rate, burst, requests from
/// scripts), moves, ages and removes particles. Emitters are independent and run in parallel
/// on the job system. State lives in ParticleStateComponent (added on first use, never saved).
void updateParticles(Scene& scene, float dt);

/// Particles alive in the scene (all emitters).
[[nodiscard]] std::size_t particleCount(const Scene& scene);

/// Ask an emitter for `count` extra particles at its next update (scripts: emitParticles).
void requestParticles(Scene& scene, unsigned entity, int count);

} // namespace lumora
