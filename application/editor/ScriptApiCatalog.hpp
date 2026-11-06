#pragma once

#include "common/ModuleExport.hpp"

#include <span>

namespace lumora::editor {

/// What the code editor offers while typing a gameplay script: Lumora's scripting API
/// (lumora/Script.hpp), so completions and the SDK cannot drift apart (a test checks every
/// Behaviour method is listed here).
struct ScriptApiEntry {
    enum class Scope {
        Behaviour,    // methods callable inside a behaviour: position(), move(v), ...
        Type,         // Vec3, Entity, LUMORA_BEHAVIOUR, ...
        Key,          // Key::W ...
        MouseButton,  // MouseButton::Left ...
        Member,       // after '.': Vec3 and RayHit fields
    };
    Scope scope;
    const char* name;       // what is typed and matched
    const char* signature;  // shown in the list, e.g. "setPosition(Vec3 v)"
    const char* doc;        // one line, shown as the tooltip and in the list
};

[[nodiscard]] LUMORA_EDITOR_API std::span<const ScriptApiEntry> scriptApi();

} // namespace lumora::editor
