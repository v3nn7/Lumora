# Scripting

A behaviour is the logic of one entity: what it does when play starts, every step, and when it
touches something. Lumora runs behaviours written three ways. Pick the one that suits you; one
scene can mix all three.

| | Visual scripts | Lua | C++ |
|---|---|---|---|
| For | everyone, no code | most gameplay code | engine-level work, heavy maths |
| File | `*.lgraph` (anywhere in the project) | `*.lua` (usually `Scripts/`) | `Scripts/*.cpp` |
| Behaviour field | `Content/Door.lgraph` | `Scripts/Player.lua` | `Player` (the class name) |
| Needs | nothing | nothing | CMake and a C++ compiler |
| Reload | on save, also while playing | on save, also while playing | after a rebuild, also while playing |

All three reach the engine through the same API table (`lumora::script::EngineApi`), so they
can do the same things: move entities, use physics, read input, play sounds and animations,
draw HUD and UI, save data, find paths, play timelines, switch scenes.

To attach a script, drag its file from the Content Browser onto an object in the viewport or the
Hierarchy. You can also add a **Script** component and type the file's project-relative path
into **Behaviour**. **Parameters** (`speed=4, jump=6`) holds numbers the script reads. Lua reads
them with `self:param("speed", 4)`, a graph with the *Script Parameter* node and C++ with
`param("speed", 4)`.

## Visual scripts

*Content Browser → New → Visual Script…* creates a small working graph and opens it in the
**Visual Script** panel.

- **Add a node:** press <kbd>Space</kbd>, right-click on the canvas or click *Add Node*, then
  type to search (for example `rot`, `key`, `raycast`) and press <kbd>Enter</kbd>.
- **Link two pins:** drag from one pin to the other. White triangles are *exec* pins and set the
  order in which actions run. Coloured circles carry values: green numbers, red booleans, yellow
  vectors, pink text, blue entities and grey "anything". A data input takes one link. Drop a
  dragged pin on empty canvas for the list of nodes that fit it; the new node comes linked.
  Dragging a linked input onto empty canvas unlinks it instead.
- **Values:** an input without a link shows a field for its value. An empty Entity input means
  "this entity"; you can also type another entity's name.
- **Variables:** add them in the table on the right and read or write them with *Get Variable*,
  *Set Variable* and *Add To Variable*. Drag a variable from the table onto the canvas for a Get
  or Set node (hold <kbd>Ctrl</kbd> for Get, <kbd>Alt</kbd> for Set).
- **Right-click** a node to delete, duplicate or unlink it, a pin to break its links, or a link
  to remove it or add a reroute point. **Delete** removes the selection. Middle mouse pans, the
  wheel zooms, and the magnifier button shows the whole graph.
- **Undo and the clipboard:** <kbd>Ctrl+Z</kbd> / <kbd>Ctrl+Shift+Z</kbd> undo and redo every
  edit; <kbd>Ctrl+C</kbd>, <kbd>Ctrl+X</kbd> and <kbd>Ctrl+V</kbd> copy, cut and paste nodes (with
  the links between them) at the mouse, <kbd>Ctrl+D</kbd> duplicates, <kbd>Ctrl+A</kbd> selects all.
- **Comments:** <kbd>C</kbd> (or *Comment*) frames the selected nodes in a titled box. Dragging its
  title moves the nodes inside; the corner resizes it; double-click the title to rename it;
  right-click for its colour.
- **Reroute points:** double-click a link to put a dot on it, then drag the dot to route the link
  around other nodes.
- **Live view:** while the game plays, the nodes and links that run light up (fading over half a
  second) and every output pin shows its last value. The panel follows the first entity running
  the graph and names it in the toolbar.
- **Save** with <kbd>Ctrl+S</kbd>. A running game reloads the graph at once.

Execution starts at event nodes: *On Start*, *On Update* (gives *delta time*), *On Collision*
(gives *other*), *On Key Pressed*, *On Action Pressed*, *Every N Seconds* and
*On Timeline Event*. Flow nodes steer it:

- *Branch*
- *Sequence*
- *For Loop*
- *Do Once*
- *Flip Flop*
- *Gate*
- *Delay* (runs its output later and does not block)

Pure nodes compute values whenever an input needs them. They cover maths, logic, vectors, text,
getters and input.

A graph that loops forever, or a node that triggers itself, is stopped, and that behaviour is
disabled with a message in the Console. The rest of the game keeps running.

The file is plain JSON (`version`, `variables`, `nodes`, `links`), so it diffs and merges well.

## Lua

*Content Browser → New → Lua Script…* writes a starter script and opens it in the **Code** panel
(with Lua highlighting).

```lua
local Player = Behaviour:extend()

function Player:start()
    self.speed = self:param("speed", 5)
    self.coins = 0
end

function Player:update(dt)
    local move = vec3(self:axis("MoveX"), 0, -self:axis("MoveY"))
    self:move(move * self.speed)                 -- Character Controller
    if self:actionPressed("Jump") then self:jump(6) end
    self:drawText("Coins: " .. self.coins, 20, 20, 28)
end

function Player:onCollision(other)
    if self:name(other) == "Coin" then
        self.coins = self.coins + 1
        self:destroy(other)
        self:playSound()
    end
end

return Player
```

A script returns its behaviour table. Fields you set on `self` belong to that one entity;
`self.entity` is its id. A plain table without `Behaviour:extend()` works too.

**Values:** `vec3(x, y, z)` with `+ - * /`, `:length()`, `:normalized()`, `:dot()`,
`:cross()`, `:distance()`, `:lerp()`. Keys and buttons are strings: `"Space"`, `"W"`,
`"LeftShift"`, `"Left"` (mouse), `"A"` (gamepad), `"LeftX"` (stick).

**Methods** (the C++ names, called with `self:`):

| Area | Methods |
|---|---|
| Entities | `name`, `find`, `findAll`, `clone`, `spawn(prefab, pos)`, `destroy`, `exists`, `loadScene`, `param` |
| Transform | `position`, `rotation`, `scale`, `setPosition`, `setRotation`, `setScale`, `moveBy`, `rotateBy`, `positionOf`, `setPositionOf` |
| Physics | `velocity`, `setVelocity`, `addForce`, `addImpulse`, `raycast(origin, dir, max)` returns `{entity, point, normal, distance}` or nil |
| Character, vehicles | `move`, `jump`, `grounded`, `drive`, `vehicleSpeed`, `setRagdoll`, `isRagdoll` |
| Input | `keyDown`, `keyPressed`, `mouseDown`, `mousePressed`, `mouseReleased`, `mouseDelta`, `mousePosition`, `action`, `actionPressed`, `axis`, `gamepadButton`, `gamepadPressed`, `gamepadAxis` |
| Time | `time`, `after(seconds, fn)`, `every(seconds, fn)`, `cancelTimer` |
| Sound | `playSound`, `stopSound`, `soundPlaying` |
| Animation | `playAnimation`, `crossfadeAnimation`, `stopAnimation`, `animationPlaying`, `setAnimFloat`, `setAnimBool`, `setAnimTrigger`, `animState` |
| Effects, looks | `emitParticles`, `setParticlesPlaying`, `particleCount`, `setColor`, `setMaterial` |
| HUD, UI | `drawRect`, `drawText`, `drawTextCentered`, `drawTextRight`, `screenSize`, `button`, `checkbox`, `slider`, `panel`, `uiStyle` |
| Debug | `log`, `warn`, `print(...)`, `debugLine`, `debugBox`, `debugSphere` |
| Navigation | `findPath`, `moveTo`, `stopMoving`, `isMoving` |
| Saves | `saveNumber`, `loadNumber`, `saveText`, `loadText`, `hasSave`, `deleteSave`, `deleteAllSaves` |
| Game | `tr`, `setLanguage`, `language`, `openGameMenu`, `openGameSettings`, `gameMenuOpen`, `playTimeline`, `stopTimeline`, `timelinePlaying`, `timelineTime`, `timelineEvent` |

The full list is in `runtime/LuaPrelude.lua`.

`require("util.math")` loads `Scripts/util/math.lua` once and returns what it returned.

Scripts run in a sandbox: there is no `io`, `os.execute`, `dofile`, `loadfile` or C module
loading, and only the clock functions of `os` remain. A call that runs longer than five seconds
is stopped.

An error disables only that behaviour. The Console shows the message with the file and line.
Save a fix and the behaviour starts again.

## C++ (advanced)

C++ behaviours are classes in `Scripts/*.cpp`, compiled by the editor into a game library. See
*Gameplay scripts (C++)* in the [guide](GUIDE.md). The API is `sdk/include/lumora/Script.hpp`.

## Animator controllers (.lanim)

An Animator can pick its clip with a state machine instead of scripts calling `playAnimation`.
*Content Browser → New → Animator Controller…* writes an example (Idle, Run and Jump). Choose it
as the Animator's **Controller**.

- **parameters** are numbers (`float`), booleans (`bool`) and one-shot `trigger`s. Scripts set
  them with `setAnimFloat("speed", v)`, `setAnimBool`, `setAnimTrigger("jump")`, or the same
  calls in Lua.
- **states** name a clip, whether it loops and its speed.
- **transitions** go from a state (or `"*"`, any state) to another one when all their
  conditions hold (`>`, `<`, `>=`, `<=`, `==`, `!=`, `true`, `false`, `trigger`). The animator
  then crossfades over `duration` seconds. `exitTime` (0..1) waits until the clip has played that
  far. A trigger is used up by the transition it fires.

`animState()` returns the current state's name.
