// The C entry points of a Lumora game library. The editor's script build includes this in
// exactly one generated source file; game code never includes it.
#pragma once

#include <lumora/Script.hpp>

#include <cstring>
#include <exception>
#include <string>

#if defined(_WIN32)
#define LUMORA_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#else
#define LUMORA_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace lumora::script::detail {
std::vector<Registration>& registry() {
    static std::vector<Registration> list;
    return list;
}
namespace {
std::string& lastError() {
    static thread_local std::string message;
    return message;
}
template <typename F>
int guarded(F&& f) {
    try {
        f();
        return 0;
    } catch (const std::exception& e) {
        lastError() = e.what();
    } catch (...) {
        lastError() = "unknown exception";
    }
    return -1;
}
} // namespace
} // namespace lumora::script::detail

LUMORA_SCRIPT_EXPORT std::uint32_t lumora_script_abi() { return LUMORA_SCRIPT_ABI; }

LUMORA_SCRIPT_EXPORT std::uint32_t lumora_script_count() {
    return static_cast<std::uint32_t>(lumora::script::detail::registry().size());
}

LUMORA_SCRIPT_EXPORT const char* lumora_script_name(std::uint32_t index) {
    const auto& list = lumora::script::detail::registry();
    return index < list.size() ? list[index].name : nullptr;
}

/// Creates behaviour `index` bound to an entity; null (see lumora_script_last_error) on failure.
LUMORA_SCRIPT_EXPORT void* lumora_script_create(std::uint32_t index, const lumora::script::EngineApi* api,
                                                lumora::script::Entity entity) {
    const auto& list = lumora::script::detail::registry();
    if (index >= list.size() || api == nullptr || api->abi != LUMORA_SCRIPT_ABI) return nullptr;
    lumora::script::Behaviour* created = nullptr;
    lumora::script::detail::guarded([&] { created = list[index].create(); });
    if (created != nullptr) lumora::script::detail::Access::bind(*created, api, entity);
    return created;
}

LUMORA_SCRIPT_EXPORT int lumora_script_start(void* instance) {
    return lumora::script::detail::guarded([&] { static_cast<lumora::script::Behaviour*>(instance)->start(); });
}

LUMORA_SCRIPT_EXPORT int lumora_script_update(void* instance, float dt) {
    return lumora::script::detail::guarded([&] {
        auto* behaviour = static_cast<lumora::script::Behaviour*>(instance);
        lumora::script::detail::Access::tick(*behaviour, dt);  // timers first, then update()
        behaviour->update(dt);
    });
}

LUMORA_SCRIPT_EXPORT int lumora_script_collision(void* instance, lumora::script::Entity other) {
    return lumora::script::detail::guarded(
        [&] { static_cast<lumora::script::Behaviour*>(instance)->onCollision(other); });
}

/// Network games: kind 0 RPC (name, data, sender), 1 player joined, 2 player left, 3 connected,
/// 4 disconnected (data = why).
LUMORA_SCRIPT_EXPORT int lumora_script_network(void* instance, int kind, int peer, const char* name, const char* data,
                                               std::uint32_t size) {
    return lumora::script::detail::guarded([&] {
        auto* b = static_cast<lumora::script::Behaviour*>(instance);
        const std::string_view text = data != nullptr ? std::string_view(data, size) : std::string_view();
        switch (kind) {
            case 0: b->onRpc(name != nullptr ? name : "", text, peer); break;
            case 1: b->onPlayerJoined(peer); break;
            case 2: b->onPlayerLeft(peer); break;
            case 3: b->onConnected(); break;
            case 4: b->onDisconnected(text); break;
            default: break;
        }
    });
}

LUMORA_SCRIPT_EXPORT void lumora_script_destroy(void* instance) {
    lumora::script::detail::guarded([&] { delete static_cast<lumora::script::Behaviour*>(instance); });
}

LUMORA_SCRIPT_EXPORT const char* lumora_script_last_error() { return lumora::script::detail::lastError().c_str(); }
