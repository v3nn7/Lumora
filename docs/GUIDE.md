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

| Panel | Use |
|---|---|
| Viewport | The scene as the engine renders it; selection, gizmos, camera |
| Hierarchy | All entities as a tree. Drag to re-parent; right-click to create, rename, duplicate or delete; filter at the top |
| Inspector | Components of the selected entity. With nothing selected it shows the scene environment |
| Content Browser | Scenes, models and textures of the project. Double-click a scene to open it; drop files here to import them |
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
  primitives (cube, sphere, cylinder, plane), lights (directional, point), a camera, an empty
  entity, and ready-made physics objects (dynamic cube and sphere, static floor).
- **Select** with a left click in the viewport or the Hierarchy. <kbd>Esc</kbd> clears the
  selection.
- **Transform** with the gizmo: <kbd>1</kbd> move, <kbd>2</kbd> rotate, <kbd>3</kbd> scale.
  Hold <kbd>Ctrl</kbd> while dragging to snap (0.25 m, 15°, 0.1×). Gizmos work in the parent's
  space, so child entities move as expected.
- **Edit** values in the Inspector. **Add Component** adds any of: Transform, Mesh Renderer,
  Light, Camera, Rigid Body, Collider, Script, Rotator. A new Collider starts out fitted to the entity's
  mesh.
- **Undo** everything with <kbd>Ctrl</kbd>+<kbd>Z</kbd> and redo with
  <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd>. <kbd>Ctrl</kbd>+<kbd>D</kbd> duplicates,
  <kbd>Delete</kbd> deletes.
- **Save** with *File → Save Scene* (<kbd>Ctrl</kbd>+<kbd>S</kbd>). Saving also updates the
  project thumbnail in the Hub.

### Models and textures

*File → Import Model* (<kbd>Ctrl</kbd>+<kbd>I</kbd>), or drop files on the Content Browser.
Supported: glTF 2.0 (`.gltf`, `.glb`) with meshes, node hierarchy, PBR materials and textures;
PNG, JPEG, TGA and BMP images. Import copies the files into `Content/` and never overwrites a
file already there. An imported model becomes entities that keep the model's node hierarchy.

Assets are referenced by keys such as `Content/Models/Helmet.glb#0` (the first mesh of that file)
or `Content/Textures/Wood.png`. The Inspector's asset pickers list what the project contains.

### Materials and lighting

The Mesh Renderer component holds the material: base colour, metallic, roughness, emissive colour
and strength, an optional procedural grid or checker pattern, and optional base-colour, metallic-roughness, normal and emissive textures. Shading is physically
based (GGX), so metallic and roughness behave as in other PBR tools.

Lights are directional (the sun) or point lights. The directional light casts soft cascaded
shadows across the whole view.

Select nothing to edit the **scene environment** in the Inspector:

| Group | Settings |
|---|---|
| Sky and Ambient | Sky zenith and horizon colours, ground colour, ambient intensity |
| Atmosphere | Height fog density and falloff |
| Post Processing | Exposure, bloom, vignette |

### Physics

Give an entity a **Collider** (box, sphere or capsule) to make it solid. Add a **Rigid Body** to
make it move:

| Motion | Behaviour |
|---|---|
| Static | Never moves (same as a Collider without a Rigid Body) |
| Kinematic | Follows its transform, for example animated by a Rotator, and pushes dynamic bodies |
| Dynamic | Simulated: gravity, collisions, mass, friction, restitution, damping |

Physics runs only in play mode. *Create → Physics* has ready-made objects to start from.

### Gameplay scripts (C++)

Behaviours are C++ classes in your project's `Scripts/` folder. The editor compiles them into a
game library and reloads it whenever you save a script, also while the game is playing.

**You need** CMake 3.20+ and a C++20 compiler: GCC or Clang on Linux
(`sudo apt install cmake g++`), Visual Studio 2022 with "Desktop development with C++" on Windows.

1. *Scripts → New Behaviour…*, enter a class name such as `Bobber`. The editor writes
   `Scripts/Bobber.cpp` from a template and opens it in your code editor.
2. Edit and save. The editor rebuilds in the background; the status bar and the Console show the
   result and any compiler errors.
3. Select an entity, *Add Component → Script*, and set **Behaviour** to `Bobber`.
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
| `find("Name")`, `positionOf(e)`, `setPositionOf(e, v)`, `name(e)` | Other entities, by name |
| `keyDown(Key::W)` | Keyboard state: WASD, QE, F, arrows, Space, Shift, Ctrl, Alt, Esc |
| `time()` | Simulated seconds since play started |
| `mouseDown(MouseButton::Left)`, `mouseDelta()` | Mouse buttons and movement this frame |
| `addForce(v)`, `addImpulse(v)`, `velocity()`, `setVelocity(v)` | Physics of a dynamic Rigid Body (newtons, newton-seconds, m/s) |
| `raycast(origin, direction, maxDistance, hit)` | Closest collider along a ray: `hit.entity`, `hit.point`, `hit.normal`, `hit.distance` |
| `onCollision(Entity other)` | Override: this entity's collider started touching another |
| `clone(e)`, `destroy(e)` | Copy an entity with its children, components and scripts; remove one at the end of the step |
| `param("speed", 4.0f)` | A number from the Script component's **Parameters** field, e.g. `speed=6, jump=4` |
| `log("…")`, `warn("…")` | Messages in the Console |

An exception thrown by a behaviour is reported in the Console and disables that behaviour; the
game keeps running. A crash in script code (for example a null pointer) takes the editor down
with it, so save your scene before experimenting. Other ways to build: *Scripts → Build Scripts*
(<kbd>Ctrl</kbd>+<kbd>B</kbd>), or `lumora-editor --headless --project <p> --build-scripts`.
The standalone player and `--simulate` use the library the editor built last
(`Saved/ScriptBuild/bin/`).

### Play mode

<kbd>F5</kbd> plays the scene inside the editor, <kbd>F6</kbd> pauses, <kbd>Shift</kbd>+<kbd>F5</kbd>
stops. Stopping restores the scene exactly as it was before Play. In play mode, <kbd>F8</kbd>
switches between the scene's camera and your editor camera.

To run a project on its own, without the editor UI:

```bash
lumora-editor --project ~/LumoraProjects/MyGame --play
```

<kbd>Esc</kbd> quits, <kbd>F11</kbd> toggles fullscreen, <kbd>F8</kbd> toggles a free camera.

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
