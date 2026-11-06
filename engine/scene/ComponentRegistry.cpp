#include "scene/ComponentRegistry.hpp"

#include "scene/Components.hpp"

#include <algorithm>

namespace lumora {

const FieldDesc* ComponentDesc::findField(std::string_view fieldId) const {
    const auto it = std::ranges::find(fields, fieldId, &FieldDesc::id);
    return it == fields.end() ? nullptr : &*it;
}

namespace {

std::string categoryOf(std::string_view id) {
    static const std::pair<std::string_view, std::string_view> kCategories[] = {
        {"MeshRenderer", "Rendering"}, {"Light", "Rendering"},       {"Decal", "Rendering"},
        {"ParticleEmitter", "Rendering"}, {"ReflectionProbe", "Rendering"}, {"LightProbeVolume", "Rendering"}, {"Terrain", "Environment"},
        {"Water", "Environment"},      {"Foliage", "Environment"},   {"RigidBody", "Physics"},
        {"Collider", "Physics"},       {"CharacterController", "Physics"}, {"Joint", "Physics"},
        {"Ragdoll", "Physics"},        {"Vehicle", "Physics"},       {"Animator", "Animation"},
        {"Skin", "Animation"},         {"ModelNode", "Animation"},   {"NavAgent", "Navigation"},
        {"AudioSource", "Audio"},      {"Script", "Scripting"},    {"Timeline", "Animation"},
    };
    for (const auto& [component, category] : kCategories) {
        if (component == id) return std::string(category);
    }
    return "Core";  // Transform, Camera, Rotator, Oscillator, and anything new
}

} // namespace

void ComponentRegistry::add(ComponentDesc desc) {
    if (desc.category == "Core") desc.category = categoryOf(desc.id);
    components_.push_back(std::move(desc));
}

const ComponentDesc* ComponentRegistry::find(std::string_view id) const {
    const auto it = std::ranges::find(components_, id, &ComponentDesc::id);
    return it == components_.end() ? nullptr : &*it;
}

namespace {

FieldDesc ranged(FieldDesc f, float min, float max, float step) {
    f.min = min;
    f.max = max;
    f.step = step;
    return f;
}

FieldDesc enumerated(FieldDesc f, std::vector<std::string> labels) {
    f.enumLabels = std::move(labels);
    return f;
}

ComponentRegistry makeBuiltin() {
    using R = ComponentRegistry;
    ComponentRegistry reg;

    {
        auto d = R::describe<TransformComponent>("Transform", "Transform");
        d.userAddable = false;
        d.removable = false;
        d.fields = {
            ranged(R::field(&TransformComponent::position, "position", "Position", FieldKind::Vec3), -1e6f, 1e6f, 0.1f),
            ranged(R::field(&TransformComponent::rotationDeg, "rotation", "Rotation", FieldKind::Vec3), -36000.0f, 36000.0f, 1.0f),
            ranged(R::field(&TransformComponent::scale, "scale", "Scale", FieldKind::Vec3), -1e4f, 1e4f, 0.05f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<MeshRendererComponent>("MeshRenderer", "Mesh Renderer");
        d.fields = {
            enumerated(R::field(&MeshRendererComponent::mesh, "mesh", "Mesh", FieldKind::Enum),
                       {"Cube", "Sphere", "Plane", "Cylinder"}),
            R::field(&MeshRendererComponent::meshAsset, "meshAsset", "Mesh Asset", FieldKind::Asset),
            R::field(&MeshRendererComponent::material, "material", "Material", FieldKind::Asset),
            R::field(&MeshRendererComponent::baseColor, "baseColor", "Base Color", FieldKind::Color),
            ranged(R::field(&MeshRendererComponent::metallic, "metallic", "Metallic", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&MeshRendererComponent::roughness, "roughness", "Roughness", FieldKind::Float), 0.02f, 1.0f, 0.01f),
            R::field(&MeshRendererComponent::emissive, "emissive", "Emissive", FieldKind::Color),
            ranged(R::field(&MeshRendererComponent::emissiveStrength, "emissiveStrength", "Emissive Strength", FieldKind::Float), 0.0f, 1000.0f, 0.1f),
            enumerated(R::field(&MeshRendererComponent::pattern, "pattern", "Pattern", FieldKind::Enum),
                       {"None", "Grid", "Checker"}),
            R::field(&MeshRendererComponent::baseColorTexture, "baseColorTexture", "Base Color Map", FieldKind::Asset),
            R::field(&MeshRendererComponent::metallicRoughnessTexture, "metallicRoughnessTexture", "Metal/Rough Map", FieldKind::Asset),
            R::field(&MeshRendererComponent::normalTexture, "normalTexture", "Normal Map", FieldKind::Asset),
            R::field(&MeshRendererComponent::emissiveTexture, "emissiveTexture", "Emissive Map", FieldKind::Asset),
            R::field(&MeshRendererComponent::occlusionTexture, "occlusionTexture", "Occlusion Map", FieldKind::Asset),
            ranged(R::field(&MeshRendererComponent::normalStrength, "normalStrength", "Normal Strength", FieldKind::Float), 0.0f, 4.0f, 0.05f),
            R::field(&MeshRendererComponent::visible, "visible", "Visible", FieldKind::Bool),
            ranged(R::field(&MeshRendererComponent::alphaCutoff, "alphaCutoff", "Alpha Cutoff (-1 = model)", FieldKind::Float), -1.0f, 1.0f, 0.01f),
            R::field(&MeshRendererComponent::doubleSided, "doubleSided", "Two-Sided", FieldKind::Bool),
            ranged(R::field(&MeshRendererComponent::opacity, "opacity", "Opacity", FieldKind::Float), 0.0f, 1.0f, 0.01f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<LightComponent>("Light", "Light");
        d.fields = {
            enumerated(R::field(&LightComponent::type, "type", "Type", FieldKind::Enum), {"Directional", "Point", "Spot"}),
            R::field(&LightComponent::color, "color", "Color", FieldKind::Color),
            ranged(R::field(&LightComponent::intensity, "intensity", "Intensity", FieldKind::Float), 0.0f, 100000.0f, 0.1f),
            ranged(R::field(&LightComponent::range, "range", "Range", FieldKind::Float), 0.01f, 10000.0f, 0.1f),
            ranged(R::field(&LightComponent::spotAngleDeg, "spotAngle", "Spot Angle", FieldKind::Float), 1.0f, 89.0f, 0.5f),
            ranged(R::field(&LightComponent::spotSoftness, "spotSoftness", "Spot Softness", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            R::field(&LightComponent::castShadows, "castShadows", "Cast Shadows", FieldKind::Bool),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<CameraComponent>("Camera", "Camera");
        d.fields = {
            ranged(R::field(&CameraComponent::fovDeg, "fov", "Field of View", FieldKind::Float), 5.0f, 170.0f, 0.5f),
            ranged(R::field(&CameraComponent::nearPlane, "near", "Near Plane", FieldKind::Float), 0.001f, 100.0f, 0.01f),
            R::field(&CameraComponent::primary, "primary", "Primary", FieldKind::Bool),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<RigidBodyComponent>("RigidBody", "Rigid Body");
        d.fields = {
            enumerated(R::field(&RigidBodyComponent::motion, "motion", "Motion", FieldKind::Enum),
                       {"Static", "Kinematic", "Dynamic"}),
            ranged(R::field(&RigidBodyComponent::mass, "mass", "Mass (kg)", FieldKind::Float), 0.001f, 1e6f, 0.1f),
            ranged(R::field(&RigidBodyComponent::friction, "friction", "Friction", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&RigidBodyComponent::restitution, "restitution", "Bounciness", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&RigidBodyComponent::linearDamping, "linearDamping", "Linear Damping", FieldKind::Float), 0.0f, 10.0f, 0.01f),
            ranged(R::field(&RigidBodyComponent::angularDamping, "angularDamping", "Angular Damping", FieldKind::Float), 0.0f, 10.0f, 0.01f),
            ranged(R::field(&RigidBodyComponent::gravityFactor, "gravityFactor", "Gravity Factor", FieldKind::Float), -10.0f, 10.0f, 0.05f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<ColliderComponent>("Collider", "Collider");
        d.fields = {
            enumerated(R::field(&ColliderComponent::shape, "shape", "Shape", FieldKind::Enum), {"Box", "Sphere", "Capsule"}),
            ranged(R::field(&ColliderComponent::size, "size", "Box Size", FieldKind::Vec3), 0.001f, 1e5f, 0.05f),
            ranged(R::field(&ColliderComponent::radius, "radius", "Radius", FieldKind::Float), 0.001f, 1e5f, 0.01f),
            ranged(R::field(&ColliderComponent::height, "height", "Height", FieldKind::Float), 0.0f, 1e5f, 0.05f),
            R::field(&ColliderComponent::offset, "offset", "Offset", FieldKind::Vec3),
            R::field(&ColliderComponent::isTrigger, "isTrigger", "Is Trigger", FieldKind::Bool),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<AnimatorComponent>("Animator", "Animator");
        d.fields = {
            R::field(&AnimatorComponent::model, "model", "Model", FieldKind::String),
            R::field(&AnimatorComponent::clip, "clip", "Clip", FieldKind::String),
            ranged(R::field(&AnimatorComponent::speed, "speed", "Speed", FieldKind::Float), -10.0f, 10.0f, 0.05f),
            R::field(&AnimatorComponent::loop, "loop", "Loop", FieldKind::Bool),
            R::field(&AnimatorComponent::playOnStart, "playOnStart", "Play on Start", FieldKind::Bool),
            R::field(&AnimatorComponent::controller, "controller", "Controller (.lanim)", FieldKind::Asset),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<SkinComponent>("Skin", "Skin");
        d.fields = {
            ranged(R::field(&SkinComponent::skin, "skin", "Skin Index", FieldKind::Int), -1.0f, 1e6f, 1.0f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<ModelNodeComponent>("ModelNode", "Model Node");
        d.fields = {
            ranged(R::field(&ModelNodeComponent::node, "node", "Node Index", FieldKind::Int), -1.0f, 1e6f, 1.0f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<AudioSourceComponent>("AudioSource", "Audio Source");
        d.fields = {
            R::field(&AudioSourceComponent::clip, "audioClip", "Clip", FieldKind::Asset),
            ranged(R::field(&AudioSourceComponent::volume, "volume", "Volume", FieldKind::Float), 0.0f, 4.0f, 0.01f),
            ranged(R::field(&AudioSourceComponent::pitch, "pitch", "Pitch", FieldKind::Float), 0.05f, 4.0f, 0.01f),
            R::field(&AudioSourceComponent::loop, "loop", "Loop", FieldKind::Bool),
            R::field(&AudioSourceComponent::playOnStart, "playOnStart", "Play on Start", FieldKind::Bool),
            R::field(&AudioSourceComponent::spatial, "spatial", "3D", FieldKind::Bool),
            ranged(R::field(&AudioSourceComponent::minDistance, "minDistance", "Min Distance", FieldKind::Float), 0.01f, 1000.0f, 0.1f),
            ranged(R::field(&AudioSourceComponent::maxDistance, "maxDistance", "Max Distance", FieldKind::Float), 0.1f, 10000.0f, 0.5f),
            enumerated(R::field(&AudioSourceComponent::bus, "bus", "Bus", FieldKind::Enum), {"Music", "SFX", "UI", "Voice", "Ambience"}),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<CharacterControllerComponent>("CharacterController", "Character Controller");
        d.fields = {
            ranged(R::field(&CharacterControllerComponent::height, "height", "Height", FieldKind::Float), 0.2f, 20.0f, 0.05f),
            ranged(R::field(&CharacterControllerComponent::radius, "radius", "Radius", FieldKind::Float), 0.05f, 5.0f, 0.01f),
            ranged(R::field(&CharacterControllerComponent::maxSlopeDeg, "maxSlope", "Max Slope (deg)", FieldKind::Float), 0.0f, 89.0f, 1.0f),
            ranged(R::field(&CharacterControllerComponent::stepHeight, "stepHeight", "Step Height", FieldKind::Float), 0.0f, 2.0f, 0.01f),
            ranged(R::field(&CharacterControllerComponent::gravityFactor, "gravityFactor", "Gravity Factor", FieldKind::Float), -10.0f, 10.0f, 0.05f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<ScriptComponent>("Script", "Script");
        d.fields = {
            R::field(&ScriptComponent::behaviour, "behaviour", "Behaviour", FieldKind::String),
            R::field(&ScriptComponent::parameters, "parameters", "Parameters", FieldKind::String),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<JointComponent>("Joint", "Joint");
        using J = JointComponent;
        d.fields = {
            enumerated(R::field(&J::type, "type", "Type", FieldKind::Enum), {"Fixed", "Hinge", "Ball", "Slider", "Distance"}),
            R::field(&J::connectedTo, "connectedTo", "Connected To", FieldKind::String),
            R::field(&J::anchor, "anchor", "Anchor", FieldKind::Vec3),
            R::field(&J::axis, "axis", "Axis", FieldKind::Vec3),
            R::field(&J::limits, "limits", "Limits", FieldKind::Bool),
            ranged(R::field(&J::minLimit, "minLimit", "Min Limit", FieldKind::Float), -180.0f, 180.0f, 0.5f),
            ranged(R::field(&J::maxLimit, "maxLimit", "Max Limit", FieldKind::Float), -180.0f, 180.0f, 0.5f),
            ranged(R::field(&J::minDistance, "minDistance", "Min Distance", FieldKind::Float), 0.0f, 1000.0f, 0.05f),
            ranged(R::field(&J::maxDistance, "maxDistance", "Max Distance", FieldKind::Float), 0.0f, 1000.0f, 0.05f),
            ranged(R::field(&J::spring, "spring", "Spring (Hz)", FieldKind::Float), 0.0f, 100.0f, 0.1f),
            ranged(R::field(&J::motorSpeed, "motorSpeed", "Motor Speed", FieldKind::Float), -10000.0f, 10000.0f, 1.0f),
            ranged(R::field(&J::motorForce, "motorForce", "Motor Force", FieldKind::Float), 0.0f, 1e7f, 1.0f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<RagdollComponent>("Ragdoll", "Ragdoll");
        d.fields = {
            R::field(&RagdollComponent::active, "active", "Active", FieldKind::Bool),
            ranged(R::field(&RagdollComponent::mass, "mass", "Mass (kg)", FieldKind::Float), 0.1f, 10000.0f, 0.5f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<VehicleComponent>("Vehicle", "Vehicle");
        using V = VehicleComponent;
        d.fields = {
            ranged(R::field(&V::wheelRadius, "wheelRadius", "Wheel Radius", FieldKind::Float), 0.05f, 5.0f, 0.01f),
            ranged(R::field(&V::wheelWidth, "wheelWidth", "Wheel Width", FieldKind::Float), 0.02f, 3.0f, 0.01f),
            ranged(R::field(&V::suspensionLength, "suspensionLength", "Suspension", FieldKind::Float), 0.0f, 3.0f, 0.01f),
            ranged(R::field(&V::engineTorque, "engineTorque", "Engine Torque", FieldKind::Float), 0.0f, 100000.0f, 10.0f),
            ranged(R::field(&V::maxSteerDeg, "maxSteer", "Max Steer", FieldKind::Float), 0.0f, 80.0f, 0.5f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<NavAgentComponent>("NavAgent", "Nav Agent");
        d.fields = {
            ranged(R::field(&NavAgentComponent::speed, "speed", "Speed", FieldKind::Float), 0.0f, 100.0f, 0.1f),
            ranged(R::field(&NavAgentComponent::acceleration, "acceleration", "Acceleration", FieldKind::Float), 0.1f, 1000.0f, 0.1f),
            ranged(R::field(&NavAgentComponent::radius, "radius", "Radius", FieldKind::Float), 0.05f, 10.0f, 0.01f),
            ranged(R::field(&NavAgentComponent::height, "height", "Height", FieldKind::Float), 0.1f, 20.0f, 0.05f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<DecalComponent>("Decal", "Decal");
        d.fields = {
            R::field(&DecalComponent::texture, "decalTexture", "Texture", FieldKind::Asset),
            R::field(&DecalComponent::color, "color", "Color", FieldKind::Color),
            ranged(R::field(&DecalComponent::opacity, "opacity", "Opacity", FieldKind::Float), 0.0f, 1.0f, 0.01f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<TerrainComponent>("Terrain", "Terrain");
        using T = TerrainComponent;
        d.fields = {
            ranged(R::field(&T::size, "size", "Size", FieldKind::Float), 1.0f, 100000.0f, 1.0f),
            ranged(R::field(&T::height, "height", "Height", FieldKind::Float), 0.0f, 10000.0f, 0.5f),
            ranged(R::field(&T::resolution, "resolution", "Resolution", FieldKind::Int), 2.0f, 1025.0f, 1.0f),
            R::field(&T::heightmap, "heightmap", "Heightmap", FieldKind::Asset),
            ranged(R::field(&T::seed, "seed", "Seed", FieldKind::Int), 0.0f, 1e6f, 1.0f),
            ranged(R::field(&T::hillSize, "hillSize", "Hill Size", FieldKind::Float), 1.0f, 100000.0f, 1.0f),
            R::field(&T::grassColor, "grassColor", "Grass Color", FieldKind::Color),
            R::field(&T::rockColor, "rockColor", "Rock Color", FieldKind::Color),
            R::field(&T::texture, "texture", "Texture", FieldKind::Asset),
            ranged(R::field(&T::textureTiling, "textureTiling", "Texture Tiling", FieldKind::Float), 0.05f, 1000.0f, 0.1f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<WaterComponent>("Water", "Water");
        using W = WaterComponent;
        d.fields = {
            ranged(R::field(&W::width, "width", "Width", FieldKind::Float), 0.1f, 100000.0f, 1.0f),
            ranged(R::field(&W::length, "length", "Length", FieldKind::Float), 0.1f, 100000.0f, 1.0f),
            R::field(&W::shallowColor, "shallowColor", "Shallow Color", FieldKind::Color),
            R::field(&W::deepColor, "deepColor", "Deep Color", FieldKind::Color),
            ranged(R::field(&W::clarity, "clarity", "Clarity", FieldKind::Float), 0.05f, 1000.0f, 0.1f),
            ranged(R::field(&W::waveHeight, "waveHeight", "Wave Height", FieldKind::Float), 0.0f, 20.0f, 0.01f),
            ranged(R::field(&W::waveLength, "waveLength", "Wave Length", FieldKind::Float), 0.1f, 1000.0f, 0.1f),
            ranged(R::field(&W::waveSpeed, "waveSpeed", "Wave Speed", FieldKind::Float), 0.0f, 20.0f, 0.05f),
            ranged(R::field(&W::buoyancy, "buoyancy", "Buoyancy", FieldKind::Float), 0.0f, 10.0f, 0.05f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<ReflectionProbeComponent>("ReflectionProbe", "Reflection Probe");
        using P = ReflectionProbeComponent;
        d.fields = {
            ranged(R::field(&P::size, "size", "Size", FieldKind::Vec3), 0.1f, 1e5f, 0.1f),
            ranged(R::field(&P::blendDistance, "blendDistance", "Blend Distance", FieldKind::Float), 0.0f, 100.0f, 0.05f),
            R::field(&P::boxProjection, "boxProjection", "Box Projection", FieldKind::Bool),
            ranged(R::field(&P::intensity, "intensity", "Intensity", FieldKind::Float), 0.0f, 16.0f, 0.05f),
            ranged(R::field(&P::resolution, "resolution", "Resolution", FieldKind::Int), 16, 1024, 16),
            R::field(&P::baked, "baked", "Baked", FieldKind::Asset),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<LightProbeVolumeComponent>("LightProbeVolume", "Light Probe Volume");
        using V = LightProbeVolumeComponent;
        d.fields = {
            ranged(R::field(&V::size, "size", "Size", FieldKind::Vec3), 0.1f, 1e5f, 0.1f),
            ranged(R::field(&V::spacing, "spacing", "Probe Spacing", FieldKind::Float), 0.25f, 100.0f, 0.05f),
            ranged(R::field(&V::blendDistance, "blendDistance", "Blend Distance", FieldKind::Float), 0.0f, 100.0f, 0.05f),
            ranged(R::field(&V::intensity, "intensity", "Intensity", FieldKind::Float), 0.0f, 16.0f, 0.05f),
            ranged(R::field(&V::bounces, "bounces", "Bounces", FieldKind::Int), 1, 8, 1),
            R::field(&V::baked, "baked", "Baked", FieldKind::Asset),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<FoliageComponent>("Foliage", "Foliage");
        using F = FoliageComponent;
        d.fields = {
            enumerated(R::field(&F::mesh, "mesh", "Mesh", FieldKind::Enum), {"Cube", "Sphere", "Plane", "Cylinder"}),
            R::field(&F::meshAsset, "meshAsset", "Mesh Asset", FieldKind::Asset),
            R::field(&F::material, "material", "Material", FieldKind::Asset),
            R::field(&F::color, "color", "Color", FieldKind::Color),
            ranged(R::field(&F::count, "count", "Count", FieldKind::Int), 0.0f, 200000.0f, 1.0f),
            ranged(R::field(&F::radius, "radius", "Radius", FieldKind::Float), 0.1f, 100000.0f, 0.5f),
            ranged(R::field(&F::seed, "seed", "Seed", FieldKind::Int), 0.0f, 1e6f, 1.0f),
            ranged(R::field(&F::minScale, "minScale", "Min Scale", FieldKind::Float), 0.01f, 100.0f, 0.05f),
            ranged(R::field(&F::maxScale, "maxScale", "Max Scale", FieldKind::Float), 0.01f, 100.0f, 0.05f),
            R::field(&F::randomRotation, "randomRotation", "Random Rotation", FieldKind::Bool),
            R::field(&F::onTerrain, "onTerrain", "On Terrain", FieldKind::Bool),
            ranged(R::field(&F::maxSlope, "maxSlope", "Max Slope", FieldKind::Float), 0.0f, 90.0f, 1.0f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<TimelineComponent>("Timeline", "Timeline");
        d.fields = {
            R::field(&TimelineComponent::timeline, "timeline", "Timeline", FieldKind::Asset),
            R::field(&TimelineComponent::playOnStart, "playOnStart", "Play On Start", FieldKind::Bool),
            R::field(&TimelineComponent::loop, "loop", "Loop", FieldKind::Bool),
            ranged(R::field(&TimelineComponent::speed, "speed", "Speed", FieldKind::Float), 0.0f, 100.0f, 0.05f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<ParticleEmitterComponent>("ParticleEmitter", "Particle Emitter");
        using P = ParticleEmitterComponent;
        d.fields = {
            R::field(&P::playing, "playing", "Playing", FieldKind::Bool),
            ranged(R::field(&P::rate, "rate", "Rate (/s)", FieldKind::Float), 0.0f, 100000.0f, 1.0f),
            ranged(R::field(&P::burst, "burst", "Burst", FieldKind::Int), 0.0f, 100000.0f, 1.0f),
            ranged(R::field(&P::maxParticles, "maxParticles", "Max Particles", FieldKind::Int), 1.0f, 100000.0f, 10.0f),
            ranged(R::field(&P::lifetime, "lifetime", "Lifetime (s)", FieldKind::Float), 0.01f, 120.0f, 0.05f),
            ranged(R::field(&P::lifetimeVariation, "lifetimeVariation", "Lifetime Variation", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&P::speed, "speed", "Speed", FieldKind::Float), 0.0f, 1000.0f, 0.05f),
            ranged(R::field(&P::speedVariation, "speedVariation", "Speed Variation", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&P::spreadDeg, "spread", "Spread", FieldKind::Float), 0.0f, 180.0f, 0.5f),
            enumerated(R::field(&P::shape, "shape", "Shape", FieldKind::Enum), {"Point", "Sphere", "Box"}),
            ranged(R::field(&P::shapeSize, "shapeSize", "Shape Size", FieldKind::Float), 0.0f, 1000.0f, 0.01f),
            ranged(R::field(&P::gravity, "gravity", "Gravity", FieldKind::Float), -10.0f, 10.0f, 0.01f),
            ranged(R::field(&P::drag, "drag", "Drag", FieldKind::Float), 0.0f, 100.0f, 0.01f),
            ranged(R::field(&P::startSize, "startSize", "Start Size", FieldKind::Float), 0.0f, 1000.0f, 0.01f),
            ranged(R::field(&P::endSize, "endSize", "End Size", FieldKind::Float), 0.0f, 1000.0f, 0.01f),
            R::field(&P::startColor, "startColor", "Start Color", FieldKind::Color),
            R::field(&P::endColor, "endColor", "End Color", FieldKind::Color),
            ranged(R::field(&P::startAlpha, "startAlpha", "Start Alpha", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&P::endAlpha, "endAlpha", "End Alpha", FieldKind::Float), 0.0f, 1.0f, 0.01f),
            ranged(R::field(&P::intensity, "intensity", "Intensity", FieldKind::Float), 0.0f, 1000.0f, 0.05f),
            enumerated(R::field(&P::blend, "blend", "Blend", FieldKind::Enum), {"Alpha", "Additive"}),
            R::field(&P::worldSpace, "worldSpace", "World Space", FieldKind::Bool),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<OscillatorComponent>("Oscillator", "Oscillator");
        d.fields = {
            R::field(&OscillatorComponent::offset, "offset", "Offset", FieldKind::Vec3),
            ranged(R::field(&OscillatorComponent::period, "period", "Period (s)", FieldKind::Float), 0.05f, 600.0f, 0.1f),
            ranged(R::field(&OscillatorComponent::phase, "phase", "Phase", FieldKind::Float), 0.0f, 1.0f, 0.01f),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<RotatorComponent>("Rotator", "Rotator");
        d.fields = {
            R::field(&RotatorComponent::axis, "axis", "Axis", FieldKind::Vec3),
            ranged(R::field(&RotatorComponent::speedDeg, "speed", "Speed (deg/s)", FieldKind::Float), -3600.0f, 3600.0f, 1.0f),
        };
        reg.add(std::move(d));
    }
    return reg;
}

} // namespace

const ComponentRegistry& ComponentRegistry::builtin() {
    static const ComponentRegistry registry = makeBuiltin();
    return registry;
}

} // namespace lumora
