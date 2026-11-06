#include "scene/ComponentRegistry.hpp"

#include "scene/Components.hpp"

#include <algorithm>

namespace lumora {

const FieldDesc* ComponentDesc::findField(std::string_view fieldId) const {
    const auto it = std::ranges::find(fields, fieldId, &FieldDesc::id);
    return it == fields.end() ? nullptr : &*it;
}

void ComponentRegistry::add(ComponentDesc desc) { components_.push_back(std::move(desc)); }

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
            R::field(&MeshRendererComponent::visible, "visible", "Visible", FieldKind::Bool),
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
