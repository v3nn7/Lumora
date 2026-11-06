#include "runtime/Simulation.hpp"

#include "runtime/AnimationSystem.hpp"
#include "runtime/AudioWorld.hpp"
#include "runtime/PhysicsWorld.hpp"
#include "runtime/Scripting.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"
#include "scene/Particles.hpp"
#include "scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>

namespace lumora {

namespace {

/// Rotates entities with a RotatorComponent around their (local) axis.
void runRotatorSystem(Scene& scene, float dt) {
    auto view = scene.registry().view<RotatorComponent, TransformComponent>();
    for (auto [e, rotator, transform] : view.each()) {
        const float len = glm::length(rotator.axis);
        if (len < 1e-6f || rotator.speedDeg == 0.0f) continue;
        const glm::vec3 axis = rotator.axis / len;
        const glm::vec3 r = glm::radians(transform.rotationDeg);
        const glm::mat4 current = glm::eulerAngleYXZ(r.y, r.x, r.z);
        const glm::mat4 delta = glm::mat4_cast(glm::angleAxis(glm::radians(rotator.speedDeg * dt), axis));
        float y = 0.0f, x = 0.0f, z = 0.0f;
        glm::extractEulerAngleYXZ(current * delta, y, x, z);
        transform.rotationDeg = glm::degrees(glm::vec3(x, y, z));
    }
}

} // namespace

void Simulation::runOscillators(float dt) {
    auto& reg = scene_.registry();
    std::erase_if(oscillatorOrigins_, [&](const auto& entry) {
        return !reg.valid(entry.first) || !reg.all_of<OscillatorComponent>(entry.first);
    });
    const double t = simulatedSeconds() + dt;  // the pose at the end of this step
    constexpr double kTwoPi = 6.283185307179586;
    for (auto [e, osc, transform] : reg.view<OscillatorComponent, TransformComponent>().each()) {
        // The start position is where the entity was when the simulation first saw it.
        const auto [it, inserted] = oscillatorOrigins_.try_emplace(e, transform.position);
        if (osc.period <= 0.0f) continue;
        const auto s = static_cast<float>(std::sin(kTwoPi * (t / osc.period + osc.phase)));
        transform.position = it->second + osc.offset * s;
    }
}

Simulation::Simulation(Scene& scene, double fixedStep)
    : scene_(scene), fixedStep_(fixedStep > 0.0 ? fixedStep : kDefaultFixedStep) {
    if (PhysicsWorld::sceneHasColliders(scene_)) physics_ = std::make_unique<PhysicsWorld>(scene_);
}

Simulation::~Simulation() = default;

int Simulation::advance(double frameSeconds) {
    accumulator_ += std::clamp(frameSeconds, 0.0, fixedStep_ * kMaxStepsPerFrame);
    int executed = 0;
    // Small tolerance so accumulated rounding error never drops a whole step.
    constexpr double kTolerance = 1e-9;
    while (accumulator_ + kTolerance >= fixedStep_ && executed < kMaxStepsPerFrame) {
        step();
        accumulator_ = std::max(0.0, accumulator_ - fixedStep_);
        ++executed;
    }
    return executed;
}

void Simulation::setScripts(std::shared_ptr<ScriptModule> module, const InputState* input) {
    if (scripts_) {
        scripts_->reload(std::move(module));
    } else if (module) {
        scripts_ = std::make_unique<ScriptRunner>(scene_, std::move(module), input);
        scripts_->setHud(&hud_);
        scripts_->setPhysics(physics_.get());
        scripts_->setAudio(audio_.get());
        scripts_->setProjectRoot(projectRoot_);
        scripts_->setAnimation(animation_.get());
    }
}

void Simulation::setProjectRoot(std::filesystem::path root) {
    projectRoot_ = std::move(root);
    if (scripts_) scripts_->setProjectRoot(projectRoot_);
    if (AnimationSystem::sceneHasAnimators(scene_)) {
        animation_ = std::make_unique<AnimationSystem>(scene_, projectRoot_);
        if (scripts_) scripts_->setAnimation(animation_.get());
    }
}

void Simulation::setAudio(std::unique_ptr<AudioWorld> audio) {
    audio_ = std::move(audio);
    if (audio_) audioDevice_ = audio_->usesDevice();
    if (scripts_) scripts_->setAudio(audio_.get());
}

std::size_t Simulation::scriptInstances() const noexcept { return scripts_ ? scripts_->instanceCount() : 0; }

void Simulation::step() {
    const auto dt = static_cast<float>(fixedStep_);
    // Behaviours first, so physics and rendering see what they did this step. They redraw the
    // HUD every step.
    if (scripts_) {
        hud_.clear();
        scripts_->update(dt, simulatedSeconds());
    }
    runRotatorSystem(scene_, dt);
    runOscillators(dt);
    if (animation_) animation_->update(dt);
    scene_.updateWorldTransforms();
    if (!physics_ && PhysicsWorld::sceneHasColliders(scene_)) {
        // Colliders appeared during play (a script cloned one): start the physics world now.
        physics_ = std::make_unique<PhysicsWorld>(scene_);
        if (scripts_) scripts_->setPhysics(physics_.get());
    }
    if (physics_) {
        physics_->step(dt);
        scene_.updateWorldTransforms();
        if (scripts_) scripts_->dispatchCollisions(physics_->takeContacts());
    }
    updateParticles(scene_, dt);  // after physics: emitters on moving bodies emit from where they are
    if (audio_) audio_->update();
    ++steps_;
    if (scripts_) {
        if (const std::string next = scripts_->takeSceneRequest(); !next.empty()) {
            if (auto loaded = loadScene(next); !loaded) log::error("Simulation", "{}", loaded.error().message);
        }
    }
}

Result<> Simulation::loadScene(const std::filesystem::path& relativePath) {
    const std::filesystem::path relative = relativePath.lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        return makeError("loadScene: {} is not inside the project", relativePath.generic_string());
    }
    auto loaded = SceneSerializer::load(projectRoot_ / relative);
    if (!loaded) return makeError("loadScene {}: {}", relative.generic_string(), loaded.error().message);
    // Systems that hold on to entities go first, the behaviours (their destructors) before all.
    if (scripts_) scripts_->restart();
    physics_.reset();
    animation_.reset();
    const bool hadAudio = audio_ != nullptr;
    audio_.reset();
    oscillatorOrigins_.clear();
    hud_.clear();
    if (auto replaced = SceneSerializer::loadInto(scene_, SceneSerializer::toJson(**loaded)); !replaced) {
        return makeError("loadScene {}: {}", relative.generic_string(), replaced.error().message);
    }
    scene_.updateWorldTransforms();
    if (PhysicsWorld::sceneHasColliders(scene_)) physics_ = std::make_unique<PhysicsWorld>(scene_);
    if (!projectRoot_.empty() && AnimationSystem::sceneHasAnimators(scene_)) {
        animation_ = std::make_unique<AnimationSystem>(scene_, projectRoot_);
    }
    if (AudioWorld::sceneHasAudio(scene_) || hadAudio) {
        auto audio = AudioWorld::create(scene_, {projectRoot_, audioDevice_});
        if (audio) {
            audio_ = std::move(*audio);
        } else {
            log::warn("Simulation", "{} - no sound in {}", audio.error().message, relative.generic_string());
        }
    }
    if (scripts_) {
        scripts_->setPhysics(physics_.get());
        scripts_->setAnimation(animation_.get());
        scripts_->setAudio(audio_.get());
    }
    steps_ = 0;
    accumulator_ = 0.0;
    loadedScene_ = relative;
    ++sceneLoads_;
    log::info("Simulation", "loaded scene {}", relative.generic_string());
    return {};
}

} // namespace lumora
