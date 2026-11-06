// Lua behaviours: gameplay scripts in Lua 5.4, the easy alternative to C++.
//
//     -- Scripts/Spinner.lua
//     local Spinner = Behaviour:extend()
//
//     function Spinner:start()
//         self.speed = self:param("speed", 90)
//     end
//
//     function Spinner:update(dt)
//         self:rotateBy(vec3(0, self.speed * dt, 0))
//         if self:keyPressed("Space") then self:jump(6) end
//     end
//
//     return Spinner
//
// Put "Scripts/Spinner.lua" into a Script component's Behaviour field. Lua scripts are not
// compiled: saving the file reloads it, also during play. They reach the engine through the same
// C function table as C++ behaviours (lumora::script::EngineApi). The full API is in
// runtime/LuaPrelude.lua (embedded) and docs/SCRIPTING.md.
//
// Scripts run in a sandbox: no io, os.execute, dofile, loadfile or C modules. require("name")
// loads Scripts/name.lua. A call that runs longer than five seconds is aborted (endless loop).
#pragma once

#include <lumora/Script.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace lumora {

class LuaHost;

/// One Lua behaviour object (a table whose metatable is the script's class).
class LuaInstance {
public:
    ~LuaInstance();
    LuaInstance(const LuaInstance&) = delete;
    LuaInstance& operator=(const LuaInstance&) = delete;

    bool start();
    bool update(float dt);
    bool collision(script::Entity other);
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& file() const noexcept { return file_; }

private:
    friend class LuaHost;
    LuaInstance(LuaHost& host, int ref, std::string file) : host_(host), ref_(ref), file_(std::move(file)) {}
    bool call(const char* method, int kind, double number);

    LuaHost& host_;
    int ref_;
    std::string file_;
    std::string error_;
};

/// One Lua state for a simulation: loads behaviour classes (once per file) and creates
/// instances of them. Instances must be destroyed before their host.
class LuaHost {
public:
    LuaHost(const script::EngineApi* api, std::filesystem::path projectRoot);
    ~LuaHost();
    LuaHost(const LuaHost&) = delete;
    LuaHost& operator=(const LuaHost&) = delete;

    /// An instance of the behaviour in `file` (project-relative, e.g. "Scripts/Player.lua")
    /// attached to `entity`; nullptr with `error` set if the script does not load.
    [[nodiscard]] std::unique_ptr<LuaInstance> create(const std::string& file, script::Entity entity, std::string& error);

    /// Files whose modification time changed since they were loaded. Their classes are
    /// forgotten, so the next create() loads the new code.
    [[nodiscard]] std::vector<std::string> changedFiles();

    /// Runs a chunk of Lua in the sandbox (tests, console). Returns false with `error` set.
    bool run(const std::string& code, std::string& error);

    /// Longest time a single call may take before it is aborted.
    void setTimeLimit(std::chrono::milliseconds limit) noexcept { timeLimit_ = limit; }

    // Used by the C callbacks.
    [[nodiscard]] const script::EngineApi* api() const noexcept { return api_; }
    [[nodiscard]] const std::filesystem::path& projectRoot() const noexcept { return root_; }
    [[nodiscard]] bool overDeadline() const noexcept { return std::chrono::steady_clock::now() > deadline_; }

private:
    friend class LuaInstance;
    bool protectedCall(int args, int results, std::string& error);  // with traceback and deadline
    int loadClass(const std::string& file, std::string& error);     // registry ref, or LUA_NOREF

    struct ClassEntry {
        int ref;
        std::filesystem::file_time_type modified;
    };
    lua_State* L_ = nullptr;
    bool ready_ = false;  // the prelude loaded
    const script::EngineApi* api_;
    std::filesystem::path root_;
    std::unordered_map<std::string, ClassEntry> classes_;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::milliseconds timeLimit_{5000};
};

} // namespace lumora
