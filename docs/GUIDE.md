# Lumora user guide

This guide walks through installing Lumora, creating a project, building a scene in the editor,
running it, and editing it together with other people. For how the engine is built inside, see
[ARCHITECTURE.md](ARCHITECTURE.md).

## Install

### With the Hub (recommended)

The Hub is a small launcher. It installs editor versions next to each other, keeps them up to
date and opens every project in the version it was made with.

1. Download the Hub from the [website](https://lumoraengine.xyz/#download):
   - **Linux:** `lumora-hub-x86_64.AppImage`. Make it executable (`chmod +x lumora-hub-x86_64.AppImage`)
     and run it.
   - **Windows:** `lumora-hub-setup.exe`. It installs for your user into
     `%LOCALAPPDATA%\Programs\Lumora Hub` without administrator rights and adds Start menu and
     desktop shortcuts. The installer is not code-signed yet, so SmartScreen may ask you to
     confirm (*More info → Run anyway*). Uninstall it from *Settings → Apps*.
   - A portable `.zip` of the Hub exists for both: unzip it anywhere and run `bin/lumora-hub`.
2. Open **Editors** and click **Install Latest**.

Editors go to `~/.local/share/Lumora/Editors/<version>/` (Linux) or
`%LOCALAPPDATA%\Lumora\Editors\<version>\` (Windows). Uninstalling the Hub leaves them and your projects
in place. Each download is checked against a signed
release feed before anything is unpacked. Updates download only the files that changed, usually
a few modules.

**Channels.** *Editors → Show* chooses which releases are offered:

| Setting | Offers |
|---|---|
| Stable (default) | Stable releases only |
| Stable and beta | Also beta and release candidates |
| Everything | Also alpha builds |

### Requirements

- Linux x86-64 with glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40, Arch).
- A GPU and driver with **Vulkan 1.4**, including dynamic rendering, synchronization2, timeline
  semaphores and descriptor indexing. Recent NVIDIA, AMD and Intel drivers and Mesa (RADV, ANV,
  NVK) qualify.

Lumora never falls back to an older Vulkan. If no GPU qualifies, the Hub still opens and the
editor names each GPU and why it was rejected. `lumora-editor --headless --info` prints the
same report.

### Build from source

You need GCC 14+ or Clang 18+, CMake 3.28+, Ninja, Qt 6.5+ (Widgets, Svg), the Vulkan 1.4 loader
and headers, and `glslc`. On Debian, Ubuntu or Parrot:

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-svg-dev libvulkan-dev vulkan-validationlayers glslc mesa-vulkan-drivers libwayland-dev
```

```bash
git clone https://github.com/v3nn7/Lumora.git && cd Lumora
cmake --preset release
cmake --build --preset release
./build/release/bin/Lumora
```

`Lumora` is the development build: it runs the Hub and the editor in one process. The first
configure downloads the remaining dependencies at pinned, hash-checked versions. To run the
tests, use the `debug` preset and `ctest --preset debug`. To use a self-built editor from an
installed Hub, choose *Editors → Add Local Build…* and pick `build/release/bin/lumora-editor`.

## Projects

### Create a project

In the Hub click **New Project**, enter a name and a location, and pick a template:

| Template | Contents |
|---|---|
| Empty | A sun light and a camera |
| Basic Scene | Ground plane, primitives and lighting |
| Material Showcase | Spheres sweeping metallic and roughness, for checking PBR shading |
| Physics Playground | A tower of crates, rolling balls and a spinning paddle; press Play |
| Platformer | A small game: run and jump (WASD, Space), collect the coins; C++ scripts in `Scripts/Game.cpp` to learn from (needs CMake and a compiler) |

The preview next to each template is rendered live by the engine. Double-click a project in the
grid to open it. **Remove from List** only forgets the project; it never deletes files.

### What is in a project

```
MyGame/
  MyGame.lumora              project descriptor (JSON)
  Content/Scenes/Main.lscene scene (JSON)
  Content/Models/            imported glTF models with their buffers and images
  Content/Textures/          imported images
  Saved/Thumbnail.png        written on save, shown in the Hub
```

Every reference inside a project is relative to the project folder. You can move, copy or commit
the folder to version control as a whole. Scenes are plain JSON and diff well.

## The editor

The editor is a set of docking panels around the viewport. Drag a panel by its title to move or
stack it; *View → Reset Layout* restores the default. The layout is remembered.

Any panel can also go into the **main area**, as a tab next to the scene: click the screen
button in its title bar or tick it under *View → Main Area*. The tab's close button (or
right-click → *Back to Dock*) returns it; <kbd>Ctrl</kbd>+<kbd>1</kbd> shows the scene again.

| Panel | Use |
|---|---|
| Viewport | The scene as the engine renders it; selection, gizmos, camera |
| Hierarchy | All entities as a tree. Drag to re-parent; right-click to create, rename, duplicate or delete; filter at the top |
| Inspector | Components of the selected entity. With nothing selected it shows the scene environment |
| Content Browser | The whole project as folders, with previews of images, models, prefabs and materials and a labelled icon for every other file type. Double-click to open folders, scenes, models, prefabs and scripts; *New* creates folders, scenes, behaviours and prefabs; *Import* or drop files to bring them in; right-click to rename, duplicate or delete (to the trash) |
| Code | Built-in C++ editor for scripts (*Open in Main Area* or <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>E</kbd> puts it in the middle as a tab next to the scene; <kbd>Ctrl</kbd>+<kbd>1</kbd> goes back to the scene): syntax colouring, line numbers, completion of the scripting API while typing (or <kbd>Ctrl</kbd>+<kbd>Space</kbd>; the keys after `Key::`, the fields after `.`), <kbd>Ctrl</kbd>+<kbd>S</kbd> saves and rebuilds, double-click a compiler error to jump to it |
| Console | Engine log |
| Profiler | Frame time and GPU time per render pass |
| Session | Live collaboration: participants, address, chat |

### Moving around

Hover over the viewport or click it, then:

| Input | Action |
|---|---|
| <kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> or arrows | Fly forward, left, back, right |
| <kbd>Q</kbd> / <kbd>E</kbd> | Down / up |
| <kbd>Shift</kbd> | Fly faster |
| Right mouse + drag | Look around. The cursor stays locked in place, also on Wayland |
| Right mouse + wheel | Change fly speed |
| Middle mouse + drag | Pan |
| Wheel | Move forward and back |
| <kbd>F</kbd> | Frame the selection |
| <kbd>G</kbd> | Toggle the grid |

*Help → Viewport Controls* shows this list inside the editor.

### Building a scene

- **Create** entities from the *Create* menu, the **+** button in the Hierarchy, or a right-click
  in the Hierarchy (*Create Child* puts the new entity under the clicked one). The menu has
  primitives (cube, sphere, cylinder, plane), lights (directional, point, spot), a camera, an empty
  entity, and ready-made physics objects (dynamic cube and sphere, static floor).
- **Select** with a left click in the viewport (while playing, clicks go to the game: select in the Hierarchy) or the Hierarchy. <kbd>Esc</kbd> clears the
  selection.
- **Go to** any file or entity with <kbd>Ctrl</kbd>+<kbd>P</kbd>: type a few letters ("plco" finds
  `PlayerController.cpp`), Enter opens the file or selects and frames the entity.
- **Select several** with <kbd>Ctrl</kbd>+click (viewport or Hierarchy) or <kbd>Shift</kbd>+click in the
  Hierarchy: the gizmo moves, rotates and scales them as a group around the last one, the
  Inspector changes a value on all of them, and duplicate and delete work on all.
- **Transform** with the gizmo: <kbd>1</kbd> move, <kbd>2</kbd> rotate, <kbd>3</kbd> scale.
  Hold <kbd>Ctrl</kbd> while dragging to snap (0.25 m, 15°, 0.1×). Gizmos work in the parent's
  space, so child entities move as expected.
- **Edit** values in the Inspector. **Add Component** adds any of: Transform, Mesh Renderer,
  Light, Camera, Rigid Body, Collider, Character Controller, Script, Audio Source, Animator, Rotator, Oscillator, Particle Emitter. A new Collider starts out fitted to the entity's
  mesh.
- **Undo** everything with <kbd>Ctrl</kbd>+<kbd>Z</kbd> and redo with
  <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd>. <kbd>Ctrl</kbd>+<kbd>D</kbd> duplicates,
  <kbd>Delete</kbd> deletes.
- **Save** with *File → Save Scene* (<kbd>Ctrl</kbd>+<kbd>S</kbd>). Saving also updates the
  project thumbnail in the Hub.

### Prefabs

A prefab is an entity with its children saved as a reusable file (`.lprefab`).

- Select the entity, then *Edit → Save Selection as Prefab…* (saved in `Content/Prefabs`).
- Double-click a prefab in the Content Browser (or right-click → *Add to Scene*, or drop one
  from outside) to place a copy where the camera looks. Undo removes it again.
- Scripts spawn them while the game runs: `spawn("Content/Prefabs/Rocket.lprefab", position)`
  returns the new entity, whose own scripts start at the end of that step.

Copies are independent: changing the prefab file later does not change copies already placed.

### Models and textures

*File → Import Model* (<kbd>Ctrl</kbd>+<kbd>I</kbd>), or drop files on the Content Browser.
Supported: glTF 2.0 (`.gltf`, `.glb`) with meshes, node hierarchy, PBR materials and textures;
PNG, JPEG, TGA and BMP images. Import copies the files into `Content/` and never overwrites a
file already there. An imported model becomes entities that keep the model's node hierarchy.

Assets are referenced by keys such as `Content/Models/Helmet.glb#0` (the first mesh of that file)
or `Content/Textures/Wood.png`. The Inspector's asset pickers list what the project contains.

### Animation

Animations in glTF models play in play mode. A model with clips gets an **Animator** on its root
entity when it is added to the scene:

| Setting | Does |
|---|---|
| Model | The glTF file the clips come from (filled in for you) |
| Clip | The clip's name; empty plays the first one |
| Speed | 1 = as authored, negative plays backwards |
| Loop, Play on Start | Repeat the clip; start it when play begins |

Clips animate the entities made from the model's nodes (they move, turn and scale); anything
the clip does not animate keeps its value. Scripts on the root entity control it with
`playAnimation("Walk")`, `stopAnimation()` and `animationPlaying()`.

An **Animator Controller** (`.lanim`, *New → Animator Controller* in the Content Browser) picks
the clip with a state machine instead: states (Idle, Run, Jump), parameters that scripts set
(`setAnimFloat("speed", v)`, `setAnimTrigger("jump")`) and transitions that crossfade between
clips when their conditions hold. Choose it as the Animator's **Controller**; see
[SCRIPTING.md](SCRIPTING.md#animator-controllers-lanim).

Skinned characters work too: meshes with a glTF skin get a **Skin** component and are deformed
on the GPU by the entities made from their joints, so a clip animating the bones (or a script
turning one) bends the mesh. Up to four joints influence each vertex.

### Materials and lighting

The Mesh Renderer component holds the material: base colour, metallic, roughness, emissive colour
and strength, an optional procedural grid or checker pattern, and optional base-colour, metallic-roughness, normal and emissive textures. Shading is physically
based (GGX), so metallic and roughness behave as in other PBR tools.

**Changed files reload by themselves**: save a model from Blender or an image from an editor into the
project and the scene shows the new version (the editor watches the project's folders).

**Material assets** (`.lmat`) share one surface between many objects. *New → Material* in the
Content Browser creates one; select it to edit it in the Inspector (changes are saved at once and
every object using it updates). Drag it onto an object in the viewport or the Hierarchy, use
*Apply to Selected Object*, or pick it in a Mesh Renderer's **Material** field. While a material
is set, the renderer's own colour and texture settings are greyed out; clear the field to use
them again. Scripts switch materials with `setMaterial()`. Models and prefabs can be dragged into
the viewport too: they land where you drop them.

Lights are directional (the sun), point or spot lights. Spot and point lights with **Cast Shadows** get shadows from a shared atlas (16 tiles: a spot
light takes one, a point light six; the lights nearest the camera come first). A spot light shines along the entity's
-Z axis in a cone (**Spot Angle** is its half-angle, **Spot Softness** how much of it fades out
at the edge). A scene can have thousands of point and spot lights:
each part of the screen only computes the ones whose range reaches it, so keep ranges tight. The directional light casts soft cascaded
shadows across the whole view. The scene's Inspector (nothing selected) also has colour grading (Contrast, Saturation,
Temperature, Tint), depth of field (blur, Focus Distance, Focus Range) and Sun Shafts, light rays
from the sun through gaps in the scene. Screen-space ambient occlusion darkens ambient light where surfaces meet
(corners, contacts, under objects).

Select nothing to edit the **scene environment** in the Inspector:

| Group | Settings |
|---|---|
| Sky and Ambient | Sky zenith and horizon colours, ground colour, ambient intensity |
| Atmosphere | Height fog density and falloff |
| Post Processing | Exposure, bloom, vignette |

### Environment maps (HDR sky)

An equirectangular **.hdr** image can replace the procedural sky and light the whole scene:
reflections, ambient light and fog all take their colour from it. Free, high-quality ones are on
[Poly Haven](https://polyhaven.com/hdris) (CC0); pick the 2K or 4K `.hdr` version.

- Drop the file on the Content Browser (or double-click one in it, or right-click → *Use as
  Sky*), or select nothing and pick it under **Environment Map → HDR Sky** in the Inspector.
- **Intensity** scales the light, **Rotation** turns the environment around the vertical axis.
- The map is prefiltered in the background when the scene opens (a fraction of a second for a
  2K image); until then the procedural sky is shown.
- The HDR carries its own sun. Keep a Directional Light for sharp shadows and align it with the
  sun in the image (rotate one or the other), or remove the light for soft, shadowless lighting.
- **Ambient** (Sky and Ambient) still scales the environment's ambient light: 1 is physically
  correct.

### Input, controllers and saved games

Behaviours can ask for **actions** instead of keys, so a game works with keyboard and mouse or a
gamepad without extra code: `action("Jump")` (held), `actionPressed("Jump")` (once per press) and
`axis("MoveX")` (-1..1). Built in are Jump, Fire, Aim, Interact, Sprint, Crouch, Pause, Submit,
Cancel, Up, Down, Left, Right and the axes MoveX, MoveY (forward is +), LookX and LookY (up is
+). *Play → Input Settings…* opens the project's `Config/Input.json`, where actions and axes are
changed or added with names such as `"Space"`, `"Mouse.Left"`, `"Gamepad.A"`,
`"Gamepad.RightTrigger"` or `"Gamepad.LeftX-"`. Game UI menus follow Up/Down/Left/Right and
Submit too, so they work with the arrow keys and a controller's d-pad.

Controllers: the first one found is used (Linux joystick devices, XInput on Windows), in the Xbox
button layout. Saved values (`saveNumber`, `saveText`) live in `Saved/GameSave.json` while
playing in the editor (*Play → Clear Saved Game Data* resets them) and in the player's data
folder in exported games.

### Navigation

Give a character a **Nav Agent** component and a script can send it anywhere it can walk:
`moveTo(target)` finds the way around walls and other agents (`isMoving()` turns false on
arrival, `stopMoving()` stops it); `findPath(from, to)` returns the corners of the way. The
navigation mesh is built from the scene's static colliders when play starts (what characters can
stand on, minus the agent's radius, steps up to 0.4 m, slopes up to 45°). An agent with a
Character Controller walks through the physics; without one it is moved directly.

### Decals

*Create → Effects → Decal* (or a **Decal** component) projects a texture or a plain colour down
the entity's -Y axis onto every surface inside its box (the entity's scale sets the size),
multiplying their colour: dirt, cracks, paint marks, footprints. **Opacity** fades it.

### Particle effects

*Create → Effects* adds ready-made **Smoke**, **Sparks** and **Fire**; any entity can get a
**Particle Emitter** component. Particles leave along the entity's +Y axis within the **Spread**
cone, from a point, sphere or box, at **Rate** per second (plus a **Burst** at the start), fall
with **Gravity** (negative rises), slow with **Drag**, and blend from the start to the end size,
colour and alpha over their **Lifetime**. **Additive** blending with an **Intensity** above 1
glows with bloom (sparks, fire); **Alpha** suits smoke and dust. Effects run in the editor as
well, so they can be tuned without playing. Scripts call `emitParticles(30)` for a burst and
`setParticlesPlaying(false)` to stop one.

### Terrain, water and foliage

**Terrain.** *Add Component → Terrain* makes ground centred on the entity: **Size** (metres per
side), **Height**, **Resolution** (vertices per side) and either a **Heightmap** (a grayscale
image in the project, white = Height) or generated hills (**Seed**, **Hill Size**). Flat ground
gets the **Grass Color**, slopes the **Rock Color**; a **Texture** tiles over it every
**Texture Tiling** metres. It is solid in play mode and characters and Nav Agents walk on it.

**Water.** A **Water** component is a surface at the entity's height, **Width** × **Length**
metres: waves (**Wave Height**, **Wave Length**, **Wave Speed**), a colour from **Shallow** to
**Deep** as the water gets deeper (**Clarity**: how far down one sees), sky reflections, sun
glints and foam where it meets the shore. Dynamic bodies float in it (**Buoyancy**; 0 turns it
off).

**Foliage.** A **Foliage** component scatters **Count** copies of a mesh (a primitive, a
**Mesh Asset** from a model, with a **Material** or **Color**) within **Radius** metres of the
entity, at random sizes and turns, standing on the terrain below and leaving ground steeper than
**Max Slope** bare. Copies are drawn with GPU instancing, so thousands cost little.

### Physics

Give an entity a **Collider** (box, sphere or capsule) to make it solid. Add a **Rigid Body** to
make it move:

| Motion | Behaviour |
|---|---|
| Static | Never moves (same as a Collider without a Rigid Body) |
| Kinematic | Follows its transform, for example animated by a Rotator, and pushes dynamic bodies |
| Dynamic | Simulated: gravity, collisions, mass, friction, restitution, damping |

**Characters.** *Add Component → Character Controller* turns an entity into a walking capsule
(height, radius, steepest walkable slope, step height): it stands on its origin, slides along
walls, climbs steps, falls and pushes dynamic bodies. Scripts steer it:

```cpp
void update(float dt) override {
    Vec3 walk{0, 0, 0};
    if (keyDown(Key::W)) walk.z -= 4.0f;
    if (keyDown(Key::S)) walk.z += 4.0f;
    if (keyDown(Key::A)) walk.x -= 4.0f;
    if (keyDown(Key::D)) walk.x += 4.0f;
    move(walk);                                    // m/s
    if (keyDown(Key::Space) && grounded()) jump(5.0f);
}
```

**Triggers.** Tick **Is Trigger** on a Collider and it blocks nothing: bodies and characters
pass through, and scripts on both sides get `onCollision`, for pickups, checkpoints and kill
zones. Characters report what they touch the same way.

**Moving platforms.** An **Oscillator** moves an entity back and forth around where it started
(**Offset**, **Period**, **Phase**). Give it a Collider and a kinematic Rigid Body and it is a
platform: dynamic bodies and characters standing on it ride along.

**Joints.** A **Joint** ties an entity's body to another body (**Connected To**: its name) or,
left empty, to the world. **Hinge** for doors and wheels (pivot **Anchor**, **Axis**, angle
limits in degrees, a **Motor** with speed in °/s and torque), **Ball** for pendulums and chains,
**Slider** for drawers and pistons (limits in metres, motor in m/s and N), **Distance** for ropes
and springs (**Min/Max Distance**, **Spring** in Hz), **Fixed** to weld two bodies. Motor and
limits can be changed while playing.

**Vehicles.** A box Collider, a dynamic Rigid Body (a car weighs about 1200 kg) and a
**Vehicle** component make a four-wheel-drive car with suspension, engine and steering; it faces
−Z. Children named `Wheel FL`, `Wheel FR`, `Wheel RL`, `Wheel RR` (for example cylinders lying on
their side) follow the wheels. A script drives it:

```cpp
void update(float dt) override {
    drive(axis("MoveY"), axis("MoveX"), keyDown(Key::S) ? 1.0f : 0.0f, keyDown(Key::Space) ? 1.0f : 0.0f);
    drawText("Speed: " + std::to_string(int(vehicleSpeed() * 3.6f)) + " km/h", 20, 20);
}
```

**Ragdolls.** Add a **Ragdoll** to an imported, skinned model and switch it on (**Active**, or
`setRagdoll(true)` from a script, for example when a character dies): every bone becomes a body
joined to its parent, the animation stops and the model falls with gravity and collisions.

Physics runs only in play mode. *Create → Physics* has ready-made objects to start from,
including a moving platform and a trigger volume.

### Gameplay scripts

Behaviours can be **visual scripts** (node graphs, no code), **Lua** scripts or **C++** classes.
Visual scripts and Lua need no compiler and reload as soon as you save them. See
[SCRIPTING.md](SCRIPTING.md) for all three; the rest of this section covers C++.

### Gameplay scripts (C++)

Behaviours are C++ classes in your project's `Scripts/` folder. The editor compiles them into a
game library and reloads it whenever you save a script, also while the game is playing.

**You need** CMake 3.20+ and a C++20 compiler: GCC or Clang on Linux
(`sudo apt install cmake g++`), Visual Studio 2022 with "Desktop development with C++" on Windows.

1. *Scripts → New Behaviour…*, enter a class name such as `Bobber`. The editor writes
   `Scripts/Bobber.cpp` from a template and opens it in the **Code** panel (*New → C++ Behaviour*
   in the Content Browser does the same; *Edit in Code Editor* opens any script or text file).
2. Edit and save. The editor rebuilds in the background; the status bar and the Console show the
   result and any compiler errors.
3. Drag `Bobber.cpp` from the Content Browser onto the entity in the viewport or the Hierarchy
   (one undo step), or select it, *Add Component → Script* and set **Behaviour** to `Bobber`.
4. Press <kbd>F5</kbd>. Change the code and save while playing: the behaviours are recreated from
   the new code and `start()` runs again, while the scene keeps its current state.

```cpp
#include <lumora/Script.hpp>
using namespace lumora::script;

class Bobber : public Behaviour {
public:
    void start() override { origin_ = position(); }
    void update(float dt) override {
        elapsed_ += dt;
        setPosition(origin_ + Vec3{0.0f, std::sin(elapsed_ * 2.0f) * 0.5f, 0.0f});
        if (keyDown(Key::Space)) setScale(Vec3{1.5f, 1.5f, 1.5f});
    }
private:
    Vec3 origin_;
    float elapsed_ = 0.0f;
};
LUMORA_BEHAVIOUR(Bobber);
```

What a behaviour can do:

| Call | Does |
|---|---|
| `start()`, `update(dt)` | Override: once when play begins (or after a reload), every fixed step (1/60 s) |
| `position()`, `rotation()`, `scale()` and `set…` | Read and change the entity's transform (metres, Euler degrees) |
| `find("Name")`, `findAll("Name")`, `positionOf(e)`, `setPositionOf(e, v)`, `name(e)` | Other entities, by name |
| `keyDown(Key::W)` | Keyboard state: letters A–Z, digits `Num0`–`Num9`, arrows, Space, Enter, Tab, Backspace, Shift, Ctrl, Alt, Esc |
| `time()` | Simulated seconds since play started |
| `mouseDown(MouseButton::Left)`, `mouseDelta()` | Mouse buttons and movement this frame |
| `addForce(v)`, `addImpulse(v)`, `velocity()`, `setVelocity(v)` | Physics of a dynamic Rigid Body (newtons, newton-seconds, m/s) |
| `raycast(origin, direction, maxDistance, hit)` | Closest collider along a ray: `hit.entity`, `hit.point`, `hit.normal`, `hit.distance` |
| `onCollision(Entity other)` | Override: this entity's collider (or character) started touching another, triggers included |
| `spawn("Content/Prefabs/X.lprefab", position)` | Add a copy of a prefab |
| `button(text, x, y, w, h)`, `checkbox(text, value, x, y)`, `slider(text, value, min, max, x, y)`, `panel(x, y, w, h)`, `uiStyle(accent)` | Game UI: menus and settings drawn over the game; a button returns true when clicked |
| `mousePosition()`, `mousePressed(b)`, `mouseReleased(b)`, `keyPressed(key)` | Mouse in HUD pixels; presses since the last step (once each) |
| `drawTextCentered(text, x, y)`, `drawTextRight(text, x, y)` | Aligned HUD text |
| `action("Jump")`, `actionPressed("Interact")`, `axis("MoveX")` | Input actions: keyboard, mouse and gamepad together (see below) |
| `gamepadConnected()`, `gamepadButton(b)`, `gamepadPressed(b)`, `gamepadAxis(a)` | A game controller directly (Xbox layout) |
| `saveNumber(key, v)`, `loadNumber(key, fallback)`, `saveText`, `loadText`, `hasSave`, `deleteSave` | Saved game data, kept between runs |
| `after(2.0f, [this] { ... })`, `every(0.5f, [this] { ... })`, `cancelTimer(id)` | Run code later or repeatedly, in game time |
| `debugLine(a, b)`, `debugBox(center, half)`, `debugSphere(center, r)` | Debug shapes on top of the scene for this step (editor while playing; player after <kbd>F3</kbd>) |
| `moveTo(target)`, `isMoving()`, `stopMoving()`, `findPath(from, to)` | Navigation (see Navigation) |
| `drive(throttle, steering, brake, handbrake)`, `vehicleSpeed()` | A Vehicle |
| `setRagdoll(on)`, `isRagdoll()` | A Ragdoll |
| `playTimeline()`, `stopTimeline()`, `timelinePlaying()`, `timelineTime()`, `timelineEvent(name)` | The entity's Timeline (cutscenes) |
| `tr("menu.play")`, `setLanguage("pl")`, `language()`, `languages()` | Texts in the player's language (see Languages) |
| `openGameMenu()`, `openGameSettings()`, `gameMenuOpen()` | The player's built-in pause and settings menu |
| `crossfadeAnimation("Run", 0.25f)` | Blend from the current clip into another (no pop) |
| `emitParticles(n)`, `setParticlesPlaying(on)`, `particleCount()` | The entity's Particle Emitter |
| `setMaterial("Content/Materials/Glow.lmat")`, `setMaterialOf(e, path)`, `setColor(v)` | Change how a Mesh Renderer looks |
| `loadScene("Content/Scenes/Level2.lscene")` | Switch to another scene at the end of the step (menus, levels); `time()` starts again from 0. In the editor, Stop returns to the scene you were editing |
| `move(velocity)`, `jump(speed)`, `grounded()` | Character Controller: walk, jump, stand |
| `clone(e)`, `destroy(e)` | Copy an entity with its children, components and scripts; remove one at the end of the step |
| `param("speed", 4.0f)` | A number from the Script component's **Parameters** field, e.g. `speed=6, jump=4` |
| `playSound()`, `stopSound()`, `soundPlaying()`, `playSoundOn(e)` | The Audio Source of this (or another) entity |
| `playAnimation("Clip")`, `stopAnimation()`, `animationPlaying()` | The Animator of this entity |
| `drawText(text, x, y, size, color)`, `drawRect(x, y, w, h, color)`, `screenSize()` | HUD on top of the game |
| `log("…")`, `warn("…")` | Messages in the Console |

**HUD.** Behaviours draw text and rectangles on top of the game, in pixels from the top-left
corner; draw them again in every `update()` (the HUD is cleared before each step):

```cpp
void update(float) override {
    const Vec3 screen = screenSize();
    drawRect(20, 20, 220 * health_, 18, Vec3{0.9f, 0.1f, 0.1f});          // health bar
    drawText("Score: " + std::to_string(score_), 20, 46, 28);           // white by default
    if (gameOver_) drawText("Game over", screen.x / 2 - 120, screen.y / 2, 56, Vec3{1, 0.8f, 0.2f});
}
```

The font covers Latin letters including Polish and most European ones.

An exception thrown by a behaviour is reported in the Console and disables that behaviour; the
game keeps running. A crash in script code (for example a null pointer) takes the editor down
with it, so save your scene before experimenting. Other ways to build: *Scripts → Build Scripts*
(<kbd>Ctrl</kbd>+<kbd>B</kbd>), or `lumora-editor --headless --project <p> --build-scripts`.
The standalone player and `--simulate` use the library the editor built last
(`Saved/ScriptBuild/bin/`).

### Cutscenes (timelines)

*Content Browser → New → Timeline* creates a `.ltimeline` and opens it in the **Timeline**
panel (next to the Content Browser). Set its **Length**, move the playhead, select an entity in
the scene and:

- **Key Transform**: its position and rotation at the playhead (the path between keys is a
  smooth curve, rotations ease in and out);
- **Key Visibility**: whether its mesh shows;
- **Animation…**: start one of its animation clips here; **Sound**: play its Audio Source;
- **Event…**: a named event scripts react to with `timelineEvent("door")`.

Tick **Preview** to see the timeline in the scene at the playhead (▶ plays it); unticking it,
saving or pressing Play puts everything back. Keys and tracks are listed below; click one to
jump there, **Delete** removes it.

A **Timeline** component plays a timeline (**Play On Start**, **Loop**, **Speed**). Tracks act
on entities by name. Scripts: `playTimeline()`, `stopTimeline()`, `timelinePlaying()`,
`timelineTime()`.

### Languages and the game menu

Put one file per language in `Content/Localization`, named by its code (`en.json`,
`pl.json`, ...), each a list of texts:

```json
{ "menu.play": "Graj", "hud.score": "Wynik", "language.name": "Polski" }
```

Scripts show `tr("menu.play")`: the text in the player's language, else English, else the key.
The language starts as the player's choice or the system's; `setLanguage("pl")`,
`language()` and `languages()` switch and list them.

**Game menu.** In the player, <kbd>Esc</kbd> opens a built-in pause menu (Resume, Settings,
Quit) when the game's behaviours do not handle Escape themselves; they can open it with
`openGameMenu()`, or its settings page with `openGameSettings()` (a main menu's "Options").
Settings: volume, graphics quality (Low, Medium, High), fullscreen, vsync and language; they are
kept per game in the player's data folder. The menu's own texts can be translated too (keys
`menu.paused`, `menu.resume`, `menu.settings`, `menu.quit`, `menu.volume`, `menu.quality`,
`menu.quality.low/medium/high`, `menu.fullscreen`, `menu.vsync`, `menu.language`, `menu.back`).

### Sound

*Add Component → Audio Source* plays a sound file (WAV, FLAC or MP3; choosing one outside the
project copies it into `Content/Audio`) while the game runs:

| Setting | Does |
|---|---|
| Clip | The sound file |
| Volume, Pitch | Loudness (1 = as recorded) and speed/pitch (1 = original) |
| Loop | Repeat until stopped |
| Play on Start | Start when play begins; otherwise a script starts it with `playSound()` |
| 3D | Heard from the entity's position, quieter with distance, panned left/right |
| Min / Max Distance | Full volume up to Min; it stops getting quieter beyond Max |
| Bus | Mixer group: Music, SFX, UI, Voice or Ambience (all below Master) |

You hear the game from the camera. Pausing play mode pauses sound. Scripts control an entity's
source with `playSound()`, `stopSound()`, `soundPlaying()` and `playSoundOn(entity)`, and the
mixer with `setBusVolume("Music", 0.5)`, `busVolume("SFX")` and `setBusMuted("Master", true)`
(an options menu with a music slider is a few lines).

### Play mode

<kbd>F5</kbd> plays the scene inside the editor, <kbd>F6</kbd> pauses, <kbd>Shift</kbd>+<kbd>F5</kbd>
stops. Stopping restores the scene exactly as it was before Play. In play mode, <kbd>F8</kbd>
switches between the scene's camera and your editor camera.

To run a project on its own, without the editor UI:

```bash
lumora-editor --project ~/LumoraProjects/MyGame --play
```

<kbd>Esc</kbd> quits (a game with behaviours gets <kbd>Esc</kbd> for its own menus; close the window to quit), <kbd>F11</kbd> toggles fullscreen, <kbd>F8</kbd> toggles a free camera, <kbd>F3</kbd> shows the behaviours' debug shapes.

### Export the game

*File → Export Game…* writes a standalone copy of the game to an empty folder: the player
runtime (the same engine and Qt libraries this editor uses), the project's content and the built
game library, plus a launcher (`run.sh` on Linux, `<Name>.bat` on Windows). Copy the folder to
any computer with a Vulkan 1.4 GPU and start the launcher; players do not need Lumora. Script
sources and editor caches stay behind. Build the scripts first if the game has any.

## Working together

Several people can edit one scene at the same time. One editor **hosts** the session and the others
**join** it. All edits go live: entities, components, hierarchy, environment, undo and redo. Each
participant sees the others' cameras and selections in their colour, and there is a chat.

### Host a session

1. Open the project, then *Collaborate → Host Session…*.
2. Enter your name, a port (default **7777**, used for TCP and UDP) and, if you like, a password.
3. Click **Start Hosting**. The *Session* panel shows the addresses others can join with.

Each address ends with the host's certificate fingerprint, for example
`lumora://192.168.1.20:7777#ab12…`. Share the whole address: a guest who joins with it is
protected against anyone impersonating your computer.

### On the same network (LAN)

Guests on the same network use one of the LAN addresses from the *Session* panel. If a firewall
runs on the host, allow TCP and UDP on the port you chose.

### Over the internet with playit.gg

You do not need to open ports on your router:

1. Install the [playit.gg](https://playit.gg) agent on the host computer and sign in.
2. Add a tunnel of type **TCP+UDP** pointing to local port **7777**. A TCP-only tunnel also works;
   cameras then travel over TCP.
3. Copy the public address playit.gg shows (for example `bright-fox.at.ply.gg:23145`) and paste it
   into the host dialog. The *Session* panel then shows it with the fingerprint and a Copy button.

### Join a session

In the Hub click **Join Live Session…**, or in an editor *Collaborate → Join Session…*. Enter the
address and your name. From the command line:

```bash
lumora-editor --join "lumora://bright-fox.at.ply.gg:23145#ab12…" --user Bob
```

A guest works in a local copy of the host's project. Models and textures the scene needs are
downloaded automatically, and files a guest imports go to the host and on to everyone else.
Saving writes each participant's own copy.

### Security

Sessions are encrypted with TLS, and camera updates are sealed with a per-participant key. The
host's certificate is created once and kept in `~/.local/share/Lumora/collab/`.

- Joining with an address that includes the fingerprint verifies the host automatically.
- Joining without it shows the host's fingerprint once. Compare it with the host's *Session*
  panel and accept; Lumora remembers it for that address.
- If a remembered host ever presents a different certificate, Lumora refuses to connect.
- The password, if set, is checked over the encrypted connection.

### When two people edit the same thing

Changes to different fields merge, even on the same entity: one person can move a cube while
another changes its colour. If two people change the same field at the same moment, the change
the host receives last wins, and everyone ends up with the same value. While a session runs,
*New Scene* and *Open Scene* are disabled so everyone stays on one scene.

## Command line

All executables accept the same options. `lumora-editor` is the installed editor; `Lumora` is the
development build.

| Command | Does |
|---|---|
| `lumora-editor --editor <project>` | Open a project in the editor (`<project>` is the folder or its `.lumora` file) |
| `lumora-editor --project <project> --play` | Run the project in the standalone player |
| `lumora-editor --editor <project> --host <port>` | Open a project and host a session |
| `lumora-editor --join <address>` | Join a session |
| `lumora-editor --headless --info` | Print GPUs, Vulkan support and why a GPU was rejected |
| `lumora-editor --headless --create <dir> --template <name>` | Create a project (`empty`, `basic`, `showcase`, `physics`) |
| `lumora-editor --headless --project <p> --import <file>` | Import a model or texture |
| `lumora-editor --headless --project <p> --build-scripts` | Compile the project's `Scripts/` |
| `lumora-editor --headless --project <p> --validate --simulate <s> --render <out.png> --width <w> --height <h>` | Check a project, simulate it, render an image |

Global options: `--gpu <index>`, `--validation` / `--no-validation`, `--no-vsync`,
`--log-level <trace|debug|info|warn|error>`, `--user <name>`, `--password <pw>`.

## Troubleshooting

**The viewport says no suitable GPU.** Run `lumora-editor --headless --info`. It lists every GPU
and the missing feature. Update the driver; on Linux, a current Mesa release provides Vulkan 1.4 on
RADV, ANV and NVK.

**The cursor does not lock on right-click (Wayland).** The compositor needs the
pointer-constraints protocol (KWin, Mutter and wlroots compositors have it). Running with
`QT_QPA_PLATFORM=xcb` uses XWayland instead.

**Nothing renders while the window is minimised.** This is intended: rendering pauses while the
compositor reports the window as hidden, for example when the screen is locked.

**A guest cannot connect.** Check that the host's port is allowed through the firewall for TCP
and UDP, and that a playit.gg tunnel points to the same port. "Certificate changed" means the
host's certificate differs from the one remembered for that address. Ask the host for the
current address with its fingerprint.

**Logs.** `~/.local/state/Lumora/logs/lumora.log`, or the Console panel. Start with
`--log-level debug` for more detail.

## Getting help and contributing

Report bugs and ideas on [GitHub Issues](https://github.com/v3nn7/Lumora/issues). Contributions
are welcome; see [CONTRIBUTING.md](https://github.com/v3nn7/Lumora/blob/main/CONTRIBUTING.md).
Lumora is free software under the GNU General Public License, version 3 or later.
