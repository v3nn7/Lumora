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
            R::field(&MeshRendererComponent::visible, "visible", "Visible", FieldKind::Bool),
        };
        reg.add(std::move(d));
    }
    {
        auto d = R::describe<LightComponent>("Light", "Light");
        d.fields = {
            enumerated(R::field(&LightComponent::type, "type", "Type", FieldKind::Enum), {"Directional", "Point"}),
            R::field(&LightComponent::color, "color", "Color", FieldKind::Color),
            ranged(R::field(&LightComponent::intensity, "intensity", "Intensity", FieldKind::Float), 0.0f, 100000.0f, 0.1f),
            ranged(R::field(&LightComponent::range, "range", "Range", FieldKind::Float), 0.01f, 10000.0f, 0.1f),
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
