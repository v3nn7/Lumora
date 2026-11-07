-- Lumora Lua API. Loaded into every Lua state before any game script; embedded into the engine
-- at build time. `engine` (the raw C functions over lumora::script::EngineApi) is passed in and
-- hidden afterwards: scripts use Behaviour methods, vec3, Key and the helpers below.
local engine = ...

---------------------------------------------------------------------------------------------
-- vec3
---------------------------------------------------------------------------------------------
local Vec3 = {}
Vec3.__index = Vec3

local function newVec3(x, y, z)
    return setmetatable({ x = x or 0, y = y or 0, z = z or 0 }, Vec3)
end

-- vec3(x, y, z), vec3(n) = (n, n, n), vec3{1, 2, 3}; also vec3.up() etc.
vec3 = setmetatable({}, {
    __call = function(_, x, y, z)
        if type(x) == "table" then return newVec3(x.x or x[1], x.y or x[2], x.z or x[3]) end
        if x ~= nil and y == nil and z == nil then return newVec3(x, x, x) end
        return newVec3(x, y, z)
    end,
})

function Vec3.__add(a, b) return newVec3(a.x + b.x, a.y + b.y, a.z + b.z) end
function Vec3.__sub(a, b) return newVec3(a.x - b.x, a.y - b.y, a.z - b.z) end
function Vec3.__unm(a) return newVec3(-a.x, -a.y, -a.z) end
function Vec3.__mul(a, b)
    if type(a) == "number" then return newVec3(a * b.x, a * b.y, a * b.z) end
    if type(b) == "number" then return newVec3(a.x * b, a.y * b, a.z * b) end
    return newVec3(a.x * b.x, a.y * b.y, a.z * b.z)
end
function Vec3.__div(a, b)
    if type(b) == "number" then return newVec3(a.x / b, a.y / b, a.z / b) end
    return newVec3(a.x / b.x, a.y / b.y, a.z / b.z)
end
function Vec3.__eq(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
function Vec3.__tostring(a) return string.format("vec3(%g, %g, %g)", a.x, a.y, a.z) end
function Vec3:length() return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end
function Vec3:normalized()
    local l = self:length()
    if l < 1e-8 then return newVec3(0, 0, 0) end
    return newVec3(self.x / l, self.y / l, self.z / l)
end
function Vec3:dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end
function Vec3:cross(b)
    return newVec3(self.y * b.z - self.z * b.y, self.z * b.x - self.x * b.z, self.x * b.y - self.y * b.x)
end
function Vec3:distance(b) return (self - b):length() end
function Vec3:lerp(b, t) return self + (b - self) * t end
function Vec3:unpack() return self.x, self.y, self.z end

vec3.zero = function() return newVec3(0, 0, 0) end
vec3.up = function() return newVec3(0, 1, 0) end
vec3.forward = function() return newVec3(0, 0, -1) end
vec3.right = function() return newVec3(1, 0, 0) end

---------------------------------------------------------------------------------------------
-- Math helpers
---------------------------------------------------------------------------------------------
function math.clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
function math.lerp(a, b, t) return a + (b - a) * t end
function math.sign(v) return v > 0 and 1 or (v < 0 and -1 or 0) end

---------------------------------------------------------------------------------------------
-- Names (keys and buttons are written as strings: "Space", "W", "Left", ...)
---------------------------------------------------------------------------------------------
local keys, mouseButtons, padButtons, padAxes = engine.names()

local function index(list, name, what)
    local i = list[name]
    if i == nil then error(string.format("unknown %s '%s'", what, tostring(name)), 3) end
    return i
end

---------------------------------------------------------------------------------------------
-- Behaviour
---------------------------------------------------------------------------------------------
Behaviour = {}
Behaviour.__index = Behaviour

--- Creates a behaviour class: local Player = Behaviour:extend()
function Behaviour:extend()
    local class = {}
    class.__index = class
    return setmetatable(class, { __index = self })
end

local function vecArgs(v) return v.x, v.y, v.z end

-- Entities and the scene
function Behaviour:name(e) return engine.name(e or self.entity) end
function Behaviour:find(name) return engine.find(name) end
function Behaviour:findAll(name) return engine.findAll(name) end
function Behaviour:clone(e) return engine.clone(e or self.entity) end
function Behaviour:spawn(prefab, position) return engine.spawn(prefab, vecArgs(position or vec3())) end
function Behaviour:destroy(e) engine.destroy(e or self.entity) end
function Behaviour:loadScene(path) return engine.loadScene(path) end
function Behaviour:param(name, fallback) return engine.parameter(self.entity, name, fallback or 0) end
function Behaviour:exists(e) return engine.getTransform(e or self.entity, 0) ~= nil end

-- Transform
local function getT(e, which) return vec3(engine.getTransform(e, which)) end
function Behaviour:position() return getT(self.entity, 0) end
function Behaviour:rotation() return getT(self.entity, 1) end
function Behaviour:scale() return getT(self.entity, 2) end
function Behaviour:positionOf(e) return getT(e, 0) end
function Behaviour:rotationOf(e) return getT(e, 1) end
function Behaviour:setPosition(v) engine.setTransform(self.entity, 0, vecArgs(v)) end
function Behaviour:setRotation(v) engine.setTransform(self.entity, 1, vecArgs(v)) end
function Behaviour:setScale(v) engine.setTransform(self.entity, 2, vecArgs(v)) end
function Behaviour:setPositionOf(e, v) engine.setTransform(e, 0, vecArgs(v)) end
function Behaviour:setRotationOf(e, v) engine.setTransform(e, 1, vecArgs(v)) end
function Behaviour:moveBy(v) self:setPosition(self:position() + v) end
function Behaviour:rotateBy(v) self:setRotation(self:rotation() + v) end

-- Physics
function Behaviour:velocity(e) local x, y, z = engine.getVelocity(e or self.entity); return vec3(x or 0, y or 0, z or 0) end
function Behaviour:setVelocity(v, e) engine.setVelocity(e or self.entity, vecArgs(v)) end
function Behaviour:addForce(v, e) engine.addForce(e or self.entity, v.x, v.y, v.z, false) end
function Behaviour:addImpulse(v, e) engine.addForce(e or self.entity, v.x, v.y, v.z, true) end
--- Returns nil, or { entity, point, normal, distance }.
function Behaviour:raycast(origin, direction, maxDistance)
    local d = direction:normalized()
    local e, px, py, pz, nx, ny, nz, dist = engine.raycast(origin.x, origin.y, origin.z, d.x, d.y, d.z, maxDistance or 100)
    if e == false then return nil end
    return { entity = e, point = vec3(px, py, pz), normal = vec3(nx, ny, nz), distance = dist }
end

-- Character controller
function Behaviour:move(velocity) engine.character(self.entity, 0, vecArgs(velocity)) end
function Behaviour:jump(speed) return engine.character(self.entity, 1, speed or 6, 0, 0) end
function Behaviour:grounded() return engine.character(self.entity, 2, 0, 0, 0) end

-- Vehicles and ragdolls
function Behaviour:drive(throttle, steering, brake, handbrake) engine.vehicle(self.entity, throttle, steering, brake or 0, handbrake or 0) end
function Behaviour:vehicleSpeed() return engine.vehicleSpeed(self.entity) end
function Behaviour:setRagdoll(active) engine.ragdoll(self.entity, active and 1 or 0) end
function Behaviour:isRagdoll() return engine.ragdoll(self.entity, 2) end

-- Input
function Behaviour:keyDown(key) return engine.keyDown(index(keys, key, "key")) end
function Behaviour:keyPressed(key) return engine.inputEdge(2, index(keys, key, "key")) end
function Behaviour:mouseDown(button) return engine.mouseDown(index(mouseButtons, button or "Left", "mouse button")) end
function Behaviour:mousePressed(button) return engine.inputEdge(0, index(mouseButtons, button or "Left", "mouse button")) end
function Behaviour:mouseReleased(button) return engine.inputEdge(1, index(mouseButtons, button or "Left", "mouse button")) end
function Behaviour:mouseDelta() local x, y = engine.mouseDelta(); return vec3(x, y, 0) end
function Behaviour:mousePosition() local x, y = engine.mousePosition(); return vec3(x, y, 0) end
function Behaviour:action(name) return engine.input(0, 0, name) ~= 0 end
function Behaviour:actionPressed(name) return engine.input(1, 0, name) ~= 0 end
function Behaviour:axis(name) return engine.input(2, 0, name) end
function Behaviour:gamepadConnected() return engine.input(3, 0, "") ~= 0 end
function Behaviour:gamepadButton(b) return engine.input(4, index(padButtons, b, "gamepad button"), "") ~= 0 end
function Behaviour:gamepadPressed(b) return engine.input(5, index(padButtons, b, "gamepad button"), "") ~= 0 end
function Behaviour:gamepadAxis(a) return engine.input(6, index(padAxes, a, "gamepad axis"), "") end

-- Time and logging
function Behaviour:time() return engine.time() end
function Behaviour:log(...) engine.log(0, table.concat(table.pack(...), " ", 1, select("#", ...))) end
function Behaviour:warn(...) engine.log(1, table.concat(table.pack(...), " ", 1, select("#", ...))) end

-- Timers (run before update; cancel with the returned id)
function Behaviour:after(seconds, fn)
    self._timers = self._timers or {}
    self._nextTimer = (self._nextTimer or 0) + 1
    self._timers[self._nextTimer] = { left = seconds, fn = fn }
    return self._nextTimer
end
function Behaviour:every(seconds, fn)
    local id = self:after(seconds, fn)
    self._timers[id].interval = math.max(seconds, 1e-3)
    return id
end
function Behaviour:cancelTimer(id) if self._timers then self._timers[id] = nil end end
function Behaviour:_tick(dt)
    local timers = self._timers
    if not timers then return end
    for id, t in pairs(timers) do
        t.left = t.left - dt
        if t.left <= 0 then
            if t.interval then t.left = t.left + t.interval else timers[id] = nil end
            t.fn(self)
        end
    end
end

-- Audio, animation, particles, rendering
function Behaviour:playSound(e) engine.sound(e or self.entity, 1) end
function Behaviour:stopSound(e) engine.sound(e or self.entity, 0) end
function Behaviour:soundPlaying(e) return engine.sound(e or self.entity, 2) end
function Behaviour:playAnimation(clip) return engine.animation(self.entity, clip or "", 1) end
function Behaviour:stopAnimation() engine.animation(self.entity, "", 0) end
function Behaviour:animationPlaying() return engine.animation(self.entity, "", 2) end
function Behaviour:crossfadeAnimation(clip, seconds) return engine.animationBlend(self.entity, clip, seconds or 0.25) end
--- Animator state machine parameters (see the Animator component's Controller).
function Behaviour:setAnimFloat(name, value) engine.animParam(self.entity, 0, name, value) end
function Behaviour:setAnimBool(name, value) engine.animParam(self.entity, 1, name, value and 1 or 0) end
function Behaviour:setAnimTrigger(name) engine.animParam(self.entity, 2, name, 1) end
function Behaviour:animState() return engine.animState(self.entity) end
function Behaviour:emitParticles(count, e) engine.particles(e or self.entity, 2, count or 20) end
function Behaviour:setParticlesPlaying(playing, e) engine.particles(e or self.entity, playing and 1 or 0, 0) end
function Behaviour:particleCount(e) return engine.particles(e or self.entity, 3, 0) end
function Behaviour:setColor(rgb, e) engine.setColor(e or self.entity, rgb.x, rgb.y, rgb.z) end
function Behaviour:setMaterial(path, e) return engine.setMaterial(e or self.entity, path or "") end

-- HUD (pixels, origin top-left; colours are vec3 0..1)
local white = { x = 1, y = 1, z = 1 }
function Behaviour:screenSize() return engine.screenSize() end
function Behaviour:drawRect(x, y, w, h, color, alpha)
    color = color or white
    engine.drawRect(x, y, w, h, color.x, color.y, color.z, alpha or 1)
end
function Behaviour:drawText(text, x, y, size, color, alpha)
    color = color or white
    engine.drawText(tostring(text), x, y, size or 24, color.x, color.y, color.z, alpha or 1)
end
function Behaviour:drawTextCentered(text, x, y, size, color, alpha)
    color = color or white
    engine.drawTextAligned(tostring(text), x, y, 0, size or 24, 1, color.x, color.y, color.z, alpha or 1)
end
function Behaviour:drawTextRight(text, x, y, size, color, alpha)
    color = color or white
    engine.drawTextAligned(tostring(text), x, y, 0, size or 24, 2, color.x, color.y, color.z, alpha or 1)
end
-- A picture: a texture of the project ("Content/UI/Heart.png"), tinted by color
function Behaviour:drawImage(texture, x, y, w, h, color, alpha)
    color = color or white
    engine.drawImage(texture, x, y, w, h, color.x, color.y, color.z, alpha or 1)
end
-- Text in a font of the project ("Content/Fonts/Title.ttf")
function Behaviour:drawTextFont(text, font, x, y, size, color, alpha)
    color = color or white
    engine.drawTextFont(tostring(text), x, y, 0, size or 24, 0, color.x, color.y, color.z, alpha or 1, font)
end
-- Audio mixer buses ("Master", "Music", "SFX", "UI", "Voice")
function Behaviour:setBusVolume(bus, volume) engine.audioBus(bus, 0, volume) end
function Behaviour:busVolume(bus) return engine.audioBus(bus, 1, 0) end
function Behaviour:setBusMuted(bus, muted) engine.audioBus(bus, 2, muted and 1 or 0) end
function Behaviour:busMuted(bus) return engine.audioBus(bus, 3, 0) ~= 0 end
-- Game UI widgets (immediate mode: call every update)
function Behaviour:button(text, x, y, w, h) return (engine.ui(0, text, x, y, w or 220, h or 48, 0, 0, 0)) end
function Behaviour:checkbox(text, value, x, y, w, h)
    local changed, v = engine.ui(1, text, x, y, w or 260, h or 40, value and 1 or 0, 0, 0)
    return v ~= 0, changed
end
function Behaviour:slider(text, value, min, max, x, y, w, h)
    local changed, v = engine.ui(2, text, x, y, w or 320, h or 40, value, min, max)
    return v, changed
end
function Behaviour:panel(x, y, w, h) engine.ui(3, "", x, y, w, h, 0, 0, 0) end
function Behaviour:uiStyle(accent, fontSize) engine.uiStyle(accent.x, accent.y, accent.z, fontSize or 22) end

-- Debug drawing (one step)
local red, green = { x = 1, y = 0, z = 0 }, { x = 0, y = 1, z = 0 }
function Behaviour:debugLine(a, b, color)
    color = color or red
    engine.debugDraw(0, a.x, a.y, a.z, b.x, b.y, b.z, color.x, color.y, color.z, 1)
end
function Behaviour:debugBox(center, halfSize, color)
    color = color or green
    engine.debugDraw(1, center.x, center.y, center.z, halfSize.x, halfSize.y, halfSize.z, color.x, color.y, color.z, 1)
end
function Behaviour:debugSphere(center, radius, color)
    color = color or green
    engine.debugDraw(2, center.x, center.y, center.z, radius, radius, radius, color.x, color.y, color.z, 1)
end

-- Navigation
function Behaviour:findPath(from, to)
    local flat = engine.findPath(from.x, from.y, from.z, to.x, to.y, to.z)
    local points = {}
    for i = 1, #flat, 3 do points[#points + 1] = vec3(flat[i], flat[i + 1], flat[i + 2]) end
    return points
end
function Behaviour:moveTo(target) return engine.navigation(self.entity, 1, vecArgs(target)) end
function Behaviour:stopMoving() engine.navigation(self.entity, 2, 0, 0, 0) end
function Behaviour:isMoving() return engine.navigation(self.entity, 3, 0, 0, 0) end

-- Saved game data
function Behaviour:saveNumber(key, value) engine.saveNumber(key, value) end
function Behaviour:loadNumber(key, fallback) return engine.loadNumber(key, fallback or 0) end
function Behaviour:saveText(key, value) engine.saveText(key, tostring(value)) end
function Behaviour:loadText(key, fallback) return engine.loadText(key, fallback or "") end
function Behaviour:hasSave(key) return engine.save(4, key) end
function Behaviour:deleteSave(key) engine.save(5, key) end
function Behaviour:deleteAllSaves() engine.save(6, "") end

-- Localization, game menu, timelines
function Behaviour:tr(key) return engine.localize(0, key) or key end
function Behaviour:setLanguage(code) return engine.localize(1, code) ~= nil end
function Behaviour:language() return engine.localize(2, "") or "" end
function Behaviour:openGameMenu() engine.gameMenu(0) end
function Behaviour:openGameSettings() engine.gameMenu(2) end
function Behaviour:gameMenuOpen() return engine.gameMenu(1) ~= 0 end
function Behaviour:playTimeline(e) return engine.timeline(e or self.entity, 0, "") ~= 0 end
function Behaviour:stopTimeline(e) engine.timeline(e or self.entity, 1, "") end
function Behaviour:timelinePlaying(e) return engine.timeline(e or self.entity, 2, "") ~= 0 end
function Behaviour:timelineTime(e) return engine.timeline(e or self.entity, 3, "") end
function Behaviour:timelineEvent(name) return engine.timeline(nil, 4, name) ~= 0 end

-- print goes to the console
function print(...)
    local parts = table.pack(...)
    for i = 1, parts.n do parts[i] = tostring(parts[i]) end
    engine.log(0, table.concat(parts, " ", 1, parts.n))
end

---------------------------------------------------------------------------------------------
-- Called by the engine
---------------------------------------------------------------------------------------------
local lumora = {}

--- Turns whatever a script file returned into a class.
function lumora.class(value, file)
    if type(value) ~= "table" then
        error(file .. " must return a behaviour table (local B = Behaviour:extend() ... return B)", 0)
    end
    if getmetatable(value) == nil then
        value.__index = value.__index or value
        setmetatable(value, { __index = Behaviour })
    end
    return value
end

function lumora.new(class, entity)
    return setmetatable({ entity = entity }, class)
end

return lumora
