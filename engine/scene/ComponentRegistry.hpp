#pragma once

#include <entt/entity/registry.hpp>
#include <glm/vec3.hpp>

#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace lumora {

/// Minimal runtime reflection for components: one description drives serialization,
/// the editor inspector and "Add Component" menus.
using FieldValue = std::variant<bool, int, float, glm::vec3, std::string>;

/// Asset: a string holding a project-relative asset reference (edited with an asset picker).
enum class FieldKind { Bool, Int, Float, Vec3, Color, String, Enum, Asset };

struct FieldDesc {
    std::string id;     // stable serialization key
    std::string label;  // UI label
    FieldKind kind = FieldKind::Float;
    float min = std::numeric_limits<float>::lowest();
    float max = std::numeric_limits<float>::max();
    float step = 0.1f;
    std::vector<std::string> enumLabels;
    std::function<FieldValue(const entt::registry&, entt::entity)> get;
    std::function<void(entt::registry&, entt::entity, const FieldValue&)> set;
};

struct ComponentDesc {
    std::string id;
    std::string label;
    /// Group in "Add Component": Core, Rendering, Environment, Physics, Animation, Navigation,
    /// Audio, Scripting.
    std::string category = "Core";
    bool userAddable = true;  // shows up in "Add Component"
    bool removable = true;
    std::vector<FieldDesc> fields;
    std::function<bool(const entt::registry&, entt::entity)> has;
    std::function<void(entt::registry&, entt::entity)> add;
    std::function<void(entt::registry&, entt::entity)> remove;

    [[nodiscard]] const FieldDesc* findField(std::string_view fieldId) const;
};

class ComponentRegistry {
public:
    /// Registry pre-populated with all built-in engine components.
    [[nodiscard]] static const ComponentRegistry& builtin();

    void add(ComponentDesc desc);
    [[nodiscard]] const std::vector<ComponentDesc>& components() const noexcept { return components_; }
    [[nodiscard]] const ComponentDesc* find(std::string_view id) const;

    template <class C>
    static ComponentDesc describe(std::string id, std::string label) {
        ComponentDesc d;
        d.id = std::move(id);
        d.label = std::move(label);
        d.has = [](const entt::registry& r, entt::entity e) { return r.all_of<C>(e); };
        d.add = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<C>(e); };
        d.remove = [](entt::registry& r, entt::entity e) { r.remove<C>(e); };
        return d;
    }

    /// Builds a field bound to a data member. Enums are exposed as int.
    template <class C, class M>
    static FieldDesc field(M C::*member, std::string id, std::string label, FieldKind kind) {
        FieldDesc f;
        f.id = std::move(id);
        f.label = std::move(label);
        f.kind = kind;
        f.get = [member](const entt::registry& r, entt::entity e) -> FieldValue {
            const M& v = r.get<C>(e).*member;
            if constexpr (std::is_enum_v<M>) {
                return static_cast<int>(v);
            } else {
                return v;
            }
        };
        f.set = [member](entt::registry& r, entt::entity e, const FieldValue& value) {
            M& dst = r.get<C>(e).*member;
            if constexpr (std::is_enum_v<M>) {
                if (const int* i = std::get_if<int>(&value)) dst = static_cast<M>(*i);
            } else {
                if (const M* v = std::get_if<M>(&value)) dst = *v;
            }
        };
        return f;
    }

private:
    std::vector<ComponentDesc> components_;
};

} // namespace lumora
