#include "runtime/LuaScript.hpp"

#include "runtime/ScriptNames.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace lumora {

namespace {

// The prelude, embedded at build time from runtime/LuaPrelude.lua.
constexpr const char kPrelude[] =
#include "LuaPrelude.inc"
    ;

using script::Entity;
using script::kNoEntity;

LuaHost& hostOf(lua_State* L) { return **static_cast<LuaHost**>(lua_getextraspace(L)); }
const script::EngineApi& api(lua_State* L) { return *hostOf(L).api(); }
void* ctx(lua_State* L) { return hostOf(L).api()->context; }

Entity checkEntity(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) return kNoEntity;
    return static_cast<Entity>(luaL_checkinteger(L, index));
}
void pushEntity(lua_State* L, Entity e) {
    if (e == kNoEntity) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(e));
    }
}
float num(lua_State* L, int index) { return static_cast<float>(luaL_checknumber(L, index)); }
float optNum(lua_State* L, int index, float fallback) { return static_cast<float>(luaL_optnumber(L, index, fallback)); }
bool truthy(lua_State* L, int index) { return lua_toboolean(L, index) != 0; }
int pushBool(lua_State* L, bool value) {
    lua_pushboolean(L, value ? 1 : 0);
    return 1;
}
int pushVec(lua_State* L, const float* v) {
    lua_pushnumber(L, v[0]);
    lua_pushnumber(L, v[1]);
    lua_pushnumber(L, v[2]);
    return 3;
}

// ---- engine.* (raw API over EngineApi) --------------------------------------------------------

int lLog(lua_State* L) {
    api(L).log(ctx(L), static_cast<int>(luaL_checkinteger(L, 1)), luaL_checkstring(L, 2));
    return 0;
}
int lFind(lua_State* L) {
    pushEntity(L, api(L).find(ctx(L), luaL_checkstring(L, 1)));
    return 1;
}
int lFindAll(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::vector<Entity> out(64);
    int total = api(L).findAll(ctx(L), name, out.data(), static_cast<int>(out.size()));
    if (total > static_cast<int>(out.size())) {
        out.resize(static_cast<std::size_t>(total));
        total = api(L).findAll(ctx(L), name, out.data(), total);
    }
    lua_createtable(L, total, 0);
    for (int i = 0; i < total; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(out[static_cast<std::size_t>(i)]));
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
int lName(lua_State* L) {
    const char* n = api(L).name(ctx(L), checkEntity(L, 1));
    if (n == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, n);
    }
    return 1;
}
int lGetTransform(lua_State* L) {
    float v[3] = {0, 0, 0};
    if (api(L).getTransform(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)), v) == 0) {
        lua_pushnil(L);
        return 1;
    }
    return pushVec(L, v);
}
int lSetTransform(lua_State* L) {
    const float v[3] = {num(L, 3), num(L, 4), num(L, 5)};
    return pushBool(L, api(L).setTransform(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)), v) != 0);
}
int lKeyDown(lua_State* L) { return pushBool(L, api(L).keyDown(ctx(L), static_cast<int>(luaL_checkinteger(L, 1))) != 0); }
int lTime(lua_State* L) {
    lua_pushnumber(L, api(L).time(ctx(L)));
    return 1;
}
int lGetVelocity(lua_State* L) {
    float v[3] = {0, 0, 0};
    if (api(L).getVelocity(ctx(L), checkEntity(L, 1), v) == 0) return 0;
    return pushVec(L, v);
}
int lSetVelocity(lua_State* L) {
    const float v[3] = {num(L, 2), num(L, 3), num(L, 4)};
    return pushBool(L, api(L).setVelocity(ctx(L), checkEntity(L, 1), v) != 0);
}
int lAddForce(lua_State* L) {
    const float v[3] = {num(L, 2), num(L, 3), num(L, 4)};
    return pushBool(L, api(L).addForce(ctx(L), checkEntity(L, 1), v, truthy(L, 5) ? 1 : 0) != 0);
}
int lRaycast(lua_State* L) {
    const float origin[3] = {num(L, 1), num(L, 2), num(L, 3)};
    const float dir[3] = {num(L, 4), num(L, 5), num(L, 6)};
    script::RayHit hit;
    if (api(L).raycast(ctx(L), origin, dir, optNum(L, 7, 100.0f), &hit) == 0) return pushBool(L, false);
    pushEntity(L, hit.entity);
    pushVec(L, &hit.point.x);
    pushVec(L, &hit.normal.x);
    lua_pushnumber(L, hit.distance);
    return 8;
}
int lClone(lua_State* L) {
    pushEntity(L, api(L).clone(ctx(L), checkEntity(L, 1)));
    return 1;
}
int lDestroy(lua_State* L) {
    api(L).destroy(ctx(L), checkEntity(L, 1));
    return 0;
}
int lMouseDown(lua_State* L) { return pushBool(L, api(L).mouseDown(ctx(L), static_cast<int>(luaL_checkinteger(L, 1))) != 0); }
int lMouseDelta(lua_State* L) {
    float xy[2] = {0, 0};
    api(L).mouseDelta(ctx(L), xy);
    lua_pushnumber(L, xy[0]);
    lua_pushnumber(L, xy[1]);
    return 2;
}
int lMousePosition(lua_State* L) {
    float xy[2] = {0, 0};
    api(L).mousePosition(ctx(L), xy);
    lua_pushnumber(L, xy[0]);
    lua_pushnumber(L, xy[1]);
    return 2;
}
int lParameter(lua_State* L) {
    lua_pushnumber(L, api(L).parameter(ctx(L), checkEntity(L, 1), luaL_checkstring(L, 2), luaL_optnumber(L, 3, 0.0)));
    return 1;
}
int lSound(lua_State* L) {
    return pushBool(L, api(L).sound(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2))) != 0);
}
int lSpawn(lua_State* L) {
    const float p[3] = {optNum(L, 2, 0), optNum(L, 3, 0), optNum(L, 4, 0)};
    pushEntity(L, api(L).spawn(ctx(L), luaL_checkstring(L, 1), p));
    return 1;
}
int lAnimation(lua_State* L) {
    return pushBool(L, api(L).animation(ctx(L), checkEntity(L, 1), luaL_optstring(L, 2, ""),
                                        static_cast<int>(luaL_checkinteger(L, 3))) != 0);
}
int lAnimationBlend(lua_State* L) {
    return pushBool(L, api(L).animationBlend(ctx(L), checkEntity(L, 1), luaL_checkstring(L, 2), optNum(L, 3, 0.25f)) != 0);
}
int lAnimParam(lua_State* L) {
    if (api(L).abi < 4 || api(L).animatorParameter == nullptr) return pushBool(L, false);
    return pushBool(L, api(L).animatorParameter(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)),
                                                luaL_checkstring(L, 3), num(L, 4)) != 0);
}
int lAnimState(lua_State* L) {
    if (api(L).abi < 4 || api(L).animatorState == nullptr) return 0;
    char buffer[256];
    const int n = api(L).animatorState(ctx(L), checkEntity(L, 1), buffer, sizeof buffer);
    if (n < 0) return 0;
    lua_pushlstring(L, buffer, static_cast<std::size_t>(std::min<int>(n, sizeof buffer - 1)));
    return 1;
}
int lCharacter(lua_State* L) {
    const float v[3] = {optNum(L, 3, 0), optNum(L, 4, 0), optNum(L, 5, 0)};
    return pushBool(L, api(L).character(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)), v) != 0);
}
int lDrawRect(lua_State* L) {
    const float r[4] = {num(L, 1), num(L, 2), num(L, 3), num(L, 4)};
    const float c[4] = {optNum(L, 5, 1), optNum(L, 6, 1), optNum(L, 7, 1), optNum(L, 8, 1)};
    api(L).drawRect(ctx(L), r, c);
    return 0;
}
int lDrawText(lua_State* L) {
    const float p[3] = {num(L, 2), num(L, 3), optNum(L, 4, 24)};
    const float c[4] = {optNum(L, 5, 1), optNum(L, 6, 1), optNum(L, 7, 1), optNum(L, 8, 1)};
    api(L).drawText(ctx(L), luaL_checkstring(L, 1), p, c);
    return 0;
}
int lDrawTextAligned(lua_State* L) {
    const float box[4] = {num(L, 2), num(L, 3), num(L, 4), optNum(L, 5, 24)};
    const float c[4] = {optNum(L, 7, 1), optNum(L, 8, 1), optNum(L, 9, 1), optNum(L, 10, 1)};
    api(L).drawTextAligned(ctx(L), luaL_checkstring(L, 1), box, static_cast<int>(luaL_checkinteger(L, 6)), c);
    return 0;
}
int lDrawImage(lua_State* L) {
    // texture, x, y, w, h [, r, g, b, a [, u0, v0, u1, v1]]
    const float r[4] = {num(L, 2), num(L, 3), num(L, 4), num(L, 5)};
    const float c[4] = {optNum(L, 6, 1), optNum(L, 7, 1), optNum(L, 8, 1), optNum(L, 9, 1)};
    const float uv[4] = {optNum(L, 10, 0), optNum(L, 11, 0), optNum(L, 12, 1), optNum(L, 13, 1)};
    api(L).drawImage(ctx(L), luaL_checkstring(L, 1), r, uv, c);
    return 0;
}
int lDrawTextFont(lua_State* L) {
    // text, x, y, width, size, align, r, g, b, a, font
    const float box[4] = {num(L, 2), num(L, 3), num(L, 4), optNum(L, 5, 24)};
    const float c[4] = {optNum(L, 7, 1), optNum(L, 8, 1), optNum(L, 9, 1), optNum(L, 10, 1)};
    api(L).drawTextFont(ctx(L), luaL_checkstring(L, 1), box, static_cast<int>(luaL_optinteger(L, 6, 0)), c, luaL_optstring(L, 11, ""));
    return 0;
}
int lUiDocument(lua_State* L) {
    // command, path, name [, value or text]
    const int command = static_cast<int>(luaL_checkinteger(L, 1));
    const char* path = luaL_optstring(L, 2, "");
    const char* name = luaL_optstring(L, 3, "");
    float value = 0.0f;
    const char* text = "";
    if (lua_isnumber(L, 4)) value = static_cast<float>(lua_tonumber(L, 4));
    else if (lua_isboolean(L, 4)) value = lua_toboolean(L, 4) ? 1.0f : 0.0f;
    else if (lua_isstring(L, 4)) text = lua_tostring(L, 4);
    if (command == 9) {
        const int length = api(L).uiDocument(ctx(L), 9, path, name, nullptr, nullptr, nullptr, 0);
        if (length <= 0) {
            lua_pushstring(L, "");
            return 1;
        }
        std::string out(static_cast<std::size_t>(length) + 1, '\0');
        api(L).uiDocument(ctx(L), 9, path, name, nullptr, nullptr, out.data(), length + 1);
        lua_pushlstring(L, out.data(), static_cast<std::size_t>(length));
        return 1;
    }
    const int result = api(L).uiDocument(ctx(L), command, path, name, &value, text, nullptr, 0);
    if (command == 5) {
        lua_pushnumber(L, value);
        return 1;
    }
    lua_pushinteger(L, result);
    return 1;
}
int lAudioBus(lua_State* L) {
    // bus, command (0 set volume, 1 get, 2 mute, 3 muted?), value
    lua_pushnumber(L, api(L).audioBus(ctx(L), luaL_checkstring(L, 1), static_cast<int>(luaL_checkinteger(L, 2)), optNum(L, 3, 0)));
    return 1;
}
int lScreenSize(lua_State* L) {
    float wh[2] = {0, 0};
    api(L).screenSize(ctx(L), wh);
    lua_pushnumber(L, wh[0]);
    lua_pushnumber(L, wh[1]);
    return 2;
}
int lLoadScene(lua_State* L) { return pushBool(L, api(L).loadScene(ctx(L), luaL_checkstring(L, 1)) != 0); }
int lSetMaterial(lua_State* L) {
    return pushBool(L, api(L).setMaterial(ctx(L), checkEntity(L, 1), luaL_optstring(L, 2, "")) != 0);
}
int lSetColor(lua_State* L) {
    const float rgb[3] = {num(L, 2), num(L, 3), num(L, 4)};
    return pushBool(L, api(L).setColor(ctx(L), checkEntity(L, 1), rgb) != 0);
}
int lInputEdge(lua_State* L) {
    return pushBool(L, api(L).inputEdge(ctx(L), static_cast<int>(luaL_checkinteger(L, 1)),
                                        static_cast<int>(luaL_checkinteger(L, 2))) != 0);
}
int lUi(lua_State* L) {
    const float rect[4] = {num(L, 3), num(L, 4), num(L, 5), num(L, 6)};
    float value = optNum(L, 7, 0);
    const float range[2] = {optNum(L, 8, 0), optNum(L, 9, 1)};
    const int result = api(L).ui(ctx(L), static_cast<int>(luaL_checkinteger(L, 1)), luaL_optstring(L, 2, ""), rect, &value, range);
    lua_pushboolean(L, result != 0);
    lua_pushnumber(L, value);
    return 2;
}
int lUiStyle(lua_State* L) {
    const float accent[3] = {num(L, 1), num(L, 2), num(L, 3)};
    api(L).uiStyle(ctx(L), accent, optNum(L, 4, 22));
    return 0;
}
int lParticles(lua_State* L) {
    lua_pushinteger(L, api(L).particles(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)),
                                        static_cast<int>(luaL_optinteger(L, 3, 0))));
    return 1;
}
int lDebugDraw(lua_State* L) {
    const float a[3] = {num(L, 2), num(L, 3), num(L, 4)};
    const float b[3] = {num(L, 5), num(L, 6), num(L, 7)};
    const float c[4] = {optNum(L, 8, 1), optNum(L, 9, 0), optNum(L, 10, 0), optNum(L, 11, 1)};
    api(L).debugDraw(ctx(L), static_cast<int>(luaL_checkinteger(L, 1)), a, b, c);
    return 0;
}
int lFindPath(lua_State* L) {
    const float a[3] = {num(L, 1), num(L, 2), num(L, 3)};
    const float b[3] = {num(L, 4), num(L, 5), num(L, 6)};
    std::vector<float> out(3 * 256);
    const int n = api(L).navigation(ctx(L), kNoEntity, 0, a, b, out.data(), 256);
    lua_createtable(L, n * 3, 0);
    for (int i = 0; i < n * 3; ++i) {
        lua_pushnumber(L, out[static_cast<std::size_t>(i)]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
int lNavigation(lua_State* L) {
    const float p[3] = {optNum(L, 3, 0), optNum(L, 4, 0), optNum(L, 5, 0)};
    return pushBool(L, api(L).navigation(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)), p, nullptr,
                                         nullptr, 0) != 0);
}
int lInput(lua_State* L) {
    lua_pushnumber(L, api(L).input(ctx(L), static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_optinteger(L, 2, 0)),
                                   luaL_optstring(L, 3, "")));
    return 1;
}
int lSaveNumber(lua_State* L) {
    api(L).save(ctx(L), 0, luaL_checkstring(L, 1), nullptr, luaL_checknumber(L, 2), nullptr, 0, nullptr);
    return 0;
}
int lLoadNumber(lua_State* L) {
    double value = luaL_optnumber(L, 2, 0.0);
    if (api(L).save(ctx(L), 1, luaL_checkstring(L, 1), nullptr, 0.0, nullptr, 0, &value) == 0) value = luaL_optnumber(L, 2, 0.0);
    lua_pushnumber(L, value);
    return 1;
}
int lSaveText(lua_State* L) {
    api(L).save(ctx(L), 2, luaL_checkstring(L, 1), luaL_checkstring(L, 2), 0.0, nullptr, 0, nullptr);
    return 0;
}
int lLoadText(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const int length = api(L).save(ctx(L), 3, key, nullptr, 0.0, nullptr, 0, nullptr);
    if (length < 0) {
        lua_pushstring(L, luaL_optstring(L, 2, ""));
        return 1;
    }
    std::string out(static_cast<std::size_t>(length) + 1, '\0');
    api(L).save(ctx(L), 3, key, nullptr, 0.0, out.data(), length + 1, nullptr);
    lua_pushlstring(L, out.data(), static_cast<std::size_t>(length));
    return 1;
}
int lSave(lua_State* L) {
    return pushBool(L, api(L).save(ctx(L), static_cast<int>(luaL_checkinteger(L, 1)), luaL_optstring(L, 2, ""), nullptr, 0.0,
                                   nullptr, 0, nullptr) != 0);
}
int lVehicle(lua_State* L) {
    const float in[4] = {optNum(L, 2, 0), optNum(L, 3, 0), optNum(L, 4, 0), optNum(L, 5, 0)};
    return pushBool(L, api(L).vehicle(ctx(L), checkEntity(L, 1), 0, in, nullptr) != 0);
}
int lVehicleSpeed(lua_State* L) {
    float out[1] = {0};
    api(L).vehicle(ctx(L), checkEntity(L, 1), 1, nullptr, out);
    lua_pushnumber(L, out[0]);
    return 1;
}
int lRagdoll(lua_State* L) {
    return pushBool(L, api(L).ragdoll(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2))) != 0);
}
int lLocalize(lua_State* L) {
    const int command = static_cast<int>(luaL_checkinteger(L, 1));
    const char* key = luaL_optstring(L, 2, "");
    const int length = api(L).localize(ctx(L), command, key, nullptr, 0);
    if (length < 0) {
        lua_pushnil(L);
        return 1;
    }
    std::string out(static_cast<std::size_t>(length) + 1, '\0');
    api(L).localize(ctx(L), command, key, out.data(), length + 1);
    lua_pushlstring(L, out.data(), static_cast<std::size_t>(length));
    return 1;
}
int lGameMenu(lua_State* L) {
    lua_pushinteger(L, api(L).gameMenu(ctx(L), static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int lTimeline(lua_State* L) {
    float out[1] = {0};
    const int r = api(L).timeline(ctx(L), checkEntity(L, 1), static_cast<int>(luaL_checkinteger(L, 2)), luaL_optstring(L, 3, ""), out);
    lua_pushnumber(L, luaL_checkinteger(L, 2) == 3 ? out[0] : static_cast<float>(r));
    return 1;
}
template <std::size_t N>
void pushNameTable(lua_State* L, const std::array<std::string_view, N>& names) {
    lua_createtable(L, 0, static_cast<int>(N));
    for (std::size_t i = 0; i < N; ++i) {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        lua_rawset(L, -3);
    }
}
int lNames(lua_State* L) {
    pushNameTable(L, script_names::kKeys);
    pushNameTable(L, script_names::kMouseButtons);
    pushNameTable(L, script_names::kGamepadButtons);
    pushNameTable(L, script_names::kGamepadAxes);
    return 4;
}

const luaL_Reg kEngine[] = {
    {"log", lLog}, {"find", lFind}, {"findAll", lFindAll}, {"name", lName},
    {"getTransform", lGetTransform}, {"setTransform", lSetTransform}, {"keyDown", lKeyDown}, {"time", lTime},
    {"getVelocity", lGetVelocity}, {"setVelocity", lSetVelocity}, {"addForce", lAddForce}, {"raycast", lRaycast},
    {"clone", lClone}, {"destroy", lDestroy}, {"mouseDown", lMouseDown}, {"mouseDelta", lMouseDelta},
    {"mousePosition", lMousePosition}, {"parameter", lParameter}, {"sound", lSound}, {"spawn", lSpawn},
    {"animation", lAnimation}, {"animationBlend", lAnimationBlend}, {"animParam", lAnimParam}, {"animState", lAnimState},
    {"character", lCharacter}, {"drawRect", lDrawRect}, {"drawText", lDrawText}, {"drawTextAligned", lDrawTextAligned},
    {"drawImage", lDrawImage}, {"drawTextFont", lDrawTextFont}, {"audioBus", lAudioBus}, {"uiDocument", lUiDocument},
    {"screenSize", lScreenSize}, {"loadScene", lLoadScene}, {"setMaterial", lSetMaterial}, {"setColor", lSetColor},
    {"inputEdge", lInputEdge}, {"ui", lUi}, {"uiStyle", lUiStyle}, {"particles", lParticles}, {"debugDraw", lDebugDraw},
    {"findPath", lFindPath}, {"navigation", lNavigation}, {"input", lInput}, {"saveNumber", lSaveNumber},
    {"loadNumber", lLoadNumber}, {"saveText", lSaveText}, {"loadText", lLoadText}, {"save", lSave},
    {"vehicle", lVehicle}, {"vehicleSpeed", lVehicleSpeed}, {"ragdoll", lRagdoll}, {"localize", lLocalize},
    {"gameMenu", lGameMenu}, {"timeline", lTimeline}, {"names", lNames}, {nullptr, nullptr},
};

// ---- Sandbox ----------------------------------------------------------------------------------

std::string readFile(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool safeRelative(std::string_view relative) {
    if (relative.empty() || relative.front() == '/' || relative.front() == '\\') return false;
    if (relative.find(':') != std::string_view::npos) return false;
    const std::filesystem::path p(relative);
    return std::none_of(p.begin(), p.end(), [](const std::filesystem::path& part) { return part == ".."; });
}

/// require("util.math") loads Scripts/util/math.lua once and returns what it returned.
int lRequire(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "lumora.modules");
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    std::string relative = name;
    std::replace(relative.begin(), relative.end(), '.', '/');
    relative = "Scripts/" + relative + ".lua";
    if (!safeRelative(relative)) return luaL_error(L, "require: invalid module name '%s'", name.c_str());
    bool ok = false;
    const std::string code = readFile(hostOf(L).projectRoot() / relative, ok);
    if (!ok) return luaL_error(L, "require: %s not found", relative.c_str());
    const std::string chunk = "@" + relative;
    if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name.c_str());
    return 1;
}

int traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (message == nullptr) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
        message = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

void watchdog(lua_State* L, lua_Debug*) {
    if (hostOf(L).overDeadline()) luaL_error(L, "the script ran for too long (an endless loop?)");
}

} // namespace

// ---- LuaHost ----------------------------------------------------------------------------------

LuaHost::LuaHost(const script::EngineApi* api, std::filesystem::path projectRoot) : api_(api), root_(std::move(projectRoot)) {
    L_ = luaL_newstate();
    if (L_ == nullptr) return;
    *static_cast<LuaHost**>(lua_getextraspace(L_)) = this;

    // Safe standard libraries only.
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},       {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                             {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
                             {LUA_OSLIBNAME, luaopen_os}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L_, lib.name, lib.func, 1);
        lua_pop(L_, 1);
    }
    // os: only the clock functions.
    lua_getglobal(L_, "os");
    for (const char* unsafe : {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"}) {
        lua_pushnil(L_);
        lua_setfield(L_, -2, unsafe);
    }
    lua_pop(L_, 1);
    for (const char* unsafe : {"dofile", "loadfile", "collectgarbage"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, unsafe);
    }
    lua_newtable(L_);
    lua_setfield(L_, LUA_REGISTRYINDEX, "lumora.modules");
    lua_pushcfunction(L_, lRequire);
    lua_setglobal(L_, "require");

    lua_sethook(L_, watchdog, LUA_MASKCOUNT, 10000);

    // Prelude: returns the `lumora` table used to build classes and instances.
    std::string error;
    lua_pushcfunction(L_, traceback);
    if (luaL_loadbufferx(L_, kPrelude, sizeof kPrelude - 1, "=LuaPrelude.lua", "t") != LUA_OK) {
        api_->log(api_->context, 2, lua_tostring(L_, -1));
        lua_pop(L_, 2);
        return;
    }
    luaL_newlib(L_, kEngine);
    deadline_ = std::chrono::steady_clock::now() + timeLimit_;
    if (lua_pcall(L_, 1, 1, -3) != LUA_OK) {
        api_->log(api_->context, 2, lua_tostring(L_, -1));
        lua_pop(L_, 2);
        return;
    }
    lua_setfield(L_, LUA_REGISTRYINDEX, "lumora.host");
    lua_pop(L_, 1);  // traceback
    ready_ = true;
}

LuaHost::~LuaHost() {
    if (L_ != nullptr) lua_close(L_);
}

bool LuaHost::protectedCall(int args, int results, std::string& error) {
    const int base = lua_gettop(L_) - args;  // the function's index
    lua_pushcfunction(L_, traceback);
    lua_insert(L_, base);
    deadline_ = std::chrono::steady_clock::now() + timeLimit_;
    const int status = lua_pcall(L_, args, results, base);
    lua_remove(L_, base);
    if (status != LUA_OK) {
        error = lua_tostring(L_, -1) != nullptr ? lua_tostring(L_, -1) : "unknown Lua error";
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaHost::run(const std::string& code, std::string& error) {
    if (L_ == nullptr) {
        error = "Lua is not available";
        return false;
    }
    if (luaL_loadbufferx(L_, code.data(), code.size(), "=chunk", "t") != LUA_OK) {
        error = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    return protectedCall(0, 0, error);
}

int LuaHost::loadClass(const std::string& file, std::string& error) {
    if (const auto it = classes_.find(file); it != classes_.end()) return it->second.ref;
    if (!safeRelative(file)) {
        error = std::format("'{}' must be a path inside the project", file);
        return LUA_NOREF;
    }
    const std::filesystem::path path = root_ / file;
    bool ok = false;
    const std::string code = readFile(path, ok);
    if (!ok) {
        error = std::format("{} not found", file);
        return LUA_NOREF;
    }
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);

    const std::string chunk = "@" + file;
    if (luaL_loadbufferx(L_, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) {
        error = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        classes_[file] = {LUA_NOREF, modified};  // remembered so the editor only reports it again after a change
        return LUA_NOREF;
    }
    if (!protectedCall(0, 1, error)) {
        classes_[file] = {LUA_NOREF, modified};
        return LUA_NOREF;
    }
    // lumora.class(result, file)
    lua_getfield(L_, LUA_REGISTRYINDEX, "lumora.host");
    lua_getfield(L_, -1, "class");
    lua_remove(L_, -2);
    lua_insert(L_, -2);
    lua_pushstring(L_, file.c_str());
    if (!protectedCall(2, 1, error)) {
        classes_[file] = {LUA_NOREF, modified};
        return LUA_NOREF;
    }
    const int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    classes_[file] = {ref, modified};
    return ref;
}

std::unique_ptr<LuaInstance> LuaHost::create(const std::string& file, Entity entity, std::string& error) {
    if (L_ == nullptr || !ready_) {
        error = "Lua is not available";
        return nullptr;
    }
    const bool cachedFailure = classes_.contains(file) && classes_[file].ref == LUA_NOREF;
    const int classRef = loadClass(file, error);
    if (classRef == LUA_NOREF) {
        if (cachedFailure && error.empty()) error = std::format("{} failed to load (fix it and save)", file);
        return nullptr;
    }
    lua_getfield(L_, LUA_REGISTRYINDEX, "lumora.host");
    lua_getfield(L_, -1, "new");
    lua_remove(L_, -2);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, classRef);
    lua_pushinteger(L_, static_cast<lua_Integer>(entity));
    if (!protectedCall(2, 1, error)) return nullptr;
    const int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return std::unique_ptr<LuaInstance>(new LuaInstance(*this, ref, file));
}

std::vector<std::string> LuaHost::changedFiles() {
    std::vector<std::string> changed;
    for (auto it = classes_.begin(); it != classes_.end();) {
        std::error_code ec;
        const auto modified = std::filesystem::last_write_time(root_ / it->first, ec);
        if (!ec && modified != it->second.modified) {
            changed.push_back(it->first);
            if (it->second.ref != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, it->second.ref);
            it = classes_.erase(it);
        } else {
            ++it;
        }
    }
    if (!changed.empty()) {
        // Modules loaded with require() are reloaded too.
        lua_newtable(L_);
        lua_setfield(L_, LUA_REGISTRYINDEX, "lumora.modules");
    }
    return changed;
}

// ---- LuaInstance ------------------------------------------------------------------------------

LuaInstance::~LuaInstance() {
    if (host_.L_ != nullptr) luaL_unref(host_.L_, LUA_REGISTRYINDEX, ref_);
}

// kind: 0 no argument, 1 number, 2 entity
bool LuaInstance::call(const char* method, int kind, double number) {
    lua_State* L = host_.L_;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
    lua_getfield(L, -1, method);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return true;  // the behaviour does not define it
    }
    lua_insert(L, -2);  // method, self
    int args = 1;
    if (kind == 1) {
        lua_pushnumber(L, number);
        ++args;
    } else if (kind == 2) {
        pushEntity(L, static_cast<Entity>(static_cast<std::uint64_t>(number)));
        ++args;
    }
    return host_.protectedCall(args, 0, error_);
}

bool LuaInstance::start() { return call("start", 0, 0.0); }

bool LuaInstance::update(float dt) {
    if (!call("_tick", 1, dt)) return false;
    return call("update", 1, dt);
}

bool LuaInstance::collision(Entity other) {
    // Entity ids fit a double exactly (EnTT ids are 32-bit).
    return call("onCollision", 2, static_cast<double>(other));
}

} // namespace lumora
