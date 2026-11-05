# Lumora Engine

Lumora is a C++23 / Vulkan 1.4 3D engine with an integrated editor. This repository holds the
first milestone: one executable (`Lumora`) that contains the **Hub** (project browser), the
**Editor** (Qt 6 docking UI around a Vulkan viewport), a standalone **Player** and a **headless**
automation mode.

The long-term goal is a high-fidelity, production-grade engine. This README separates what
works today from what is planned, and nothing listed as planned exists yet.

## What works today

| Area | Status |
|---|---|
| Build | CMake + Ninja presets, pinned and hash-verified FetchContent dependencies, HLSL → SPIR-V at build time |
| Application | One `Lumora` executable; Hub → Editor → Hub transitions in one process; Player and headless modes |
| Hub | Hero banner rendered live by the engine, recent projects grid with thumbnails, search and sort, project creation from 3 templates with live-rendered previews (saved as the project thumbnail), project settings, GPU/engine info |
| Editor | Embedded Vulkan viewport, fly camera, click picking with selection highlight, move/rotate/scale gizmos (snapping, parent-space aware), undo/redo for every edit, hierarchy (drag & drop re-parenting, rename, duplicate, delete, filter), reflection-driven inspector with asset pickers, content browser (open scenes, add/drop-import models), console, GPU profiler, play/pause/stop, scene save/load, dockable and persisted layout |
| Collaboration | Live multi-user editing of one scene: host on a port (TCP + UDP), others join by LAN address or a playit.gg tunnel address; edits merge per field, collaborators' cameras and selections are drawn in their colour, chat, missing models/textures are transferred automatically, optional password |
| Renderer | Vulkan 1.4 instance/device with capability detection, VMA, dynamic rendering, synchronization2, render graph with automatic barriers, bindless descriptors (update-after-bind), timeline-semaphore uploads, persistent pipeline cache, GPU timestamp profiling, frames in flight, swapchain recreation, device-loss reporting |
| Rendering | HDR forward PBR (GGX / Smith / Schlick) with base colour, metallic-roughness, normal and emissive maps (mipmapped, anisotropic), directional + point lights, cascaded sun shadows (4 texel-snapped cascades in one atlas, blended borders, smooth 7x7 PCF with receiver-plane bias; off-screen casters included), procedural sky with horizon haze and sun glow, exponential height fog (aerial perspective), physically based bloom, vignette, ACES tonemapping, procedural grid/checker surface patterns, hemispherical ambient, editor grid and gizmo overlay pass, CPU frustum culling |
| Scene | EnTT ECS, UUID identity, hierarchy, component reflection → JSON serialization + inspector |
| Assets | glTF 2.0 import (.gltf/.glb: meshes, node hierarchy, PBR materials and textures), PNG/JPEG/TGA/BMP textures, portable project-relative asset references, import copies files into the project without overwriting |
| Runtime | Fixed-timestep simulation (deterministic), built-in `Rotator` behaviour |
| Tests | 151 GoogleTest cases: core, scene, serialization, scene patches, projects, CLI, glTF/texture import (incl. a locale regression), render-graph barrier planning, gizmo math, undo/redo, **GPU tests that render real frames and check pixels**, Hub/Editor UI integration tests, collaboration over real loopback sockets (convergence, assets, presence, hostile input) |

Verified on Parrot OS 7.4 (KDE Plasma, Wayland) with an NVIDIA GTX 1650 (Mesa NVK, Vulkan
1.4.354) and on Mesa llvmpipe. Validation layers and GPU-assisted validation report no errors.
Release build: about 60 fps paced to the display, about 500 fps uncapped in the template scenes.

## Requirements

- Linux (Windows code paths exist but have not been built yet), GCC 14+ or Clang 18+ (C++23)
- CMake ≥ 3.28, Ninja
- Qt ≥ 6.5 (Widgets, Svg) with Vulkan support (standard on Debian/Parrot Qt 6 builds)
- Vulkan 1.4 loader + headers, `glslc` (shaderc)
- A GPU/driver exposing **Vulkan 1.4** with dynamic rendering, synchronization2, timeline
  semaphores and update-after-bind descriptor indexing

Lumora does not fall back to an older Vulkan version. If no device qualifies, the Hub still
works, and the editor viewport and `--headless --info` name each GPU and the reason it was
rejected.

Parrot OS / Debian:

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-svg-dev libvulkan-dev vulkan-validationlayers glslc mesa-vulkan-drivers libwayland-dev
```

The first configure downloads GLM, EnTT, Vulkan Memory Allocator, nlohmann/json, cgltf, stb,
Jolt, GoogleTest and the wayland-protocols XML (pinned versions, SHA-256 verified).
`libwayland-dev` (wayland-client + wayland-scanner) is optional: without it the mouse look cannot
lock the cursor on Wayland.

## Build

```bash
cmake --preset release
```

```bash
cmake --build --preset release
```

The executable is `build/release/bin/Lumora`; compiled shaders are written to `build/release/bin/shaders/`.
Use `debug` instead of `release` for a Debug build, which also enables Vulkan validation by default.

Run the tests (the GPU tests are skipped, not faked, when no Vulkan 1.4 device exists):

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

## Usage

```bash
./build/release/bin/Lumora
```

| Command | Mode |
|---|---|
| `Lumora` | Hub |
| `Lumora --editor <project>` | Editor (`<project>` = project folder or `.lumora` file) |
| `Lumora --project <project> --play` | Standalone player (Esc quits, F11 fullscreen, F8 free camera) |
| `Lumora --headless [actions]` | Automation, no window |
| `Lumora --join <host:port>` | Join a live session (LAN address or playit.gg address) |
| `Lumora --editor <project> --host <port>` | Open a project and host a live session |

Headless examples:

```bash
./build/release/bin/Lumora --headless --info
```

```bash
./build/release/bin/Lumora --headless --create ~/LumoraProjects/Demo --template showcase
```

```bash
./build/release/bin/Lumora --headless --project ~/LumoraProjects/Demo --import ~/Downloads/DamagedHelmet.glb
```

```bash
./build/release/bin/Lumora --headless --project ~/LumoraProjects/Demo --validate --simulate 2 --render demo.png --width 1920 --height 1080
```

Global options: `--gpu <index>`, `--validation` / `--no-validation`, `--no-vsync`,
`--log-level <trace|debug|info|warn|error>`, `--user <name>` and `--password <pw>` (for `--join` /
`--host`). Logs also go to `~/.local/state/Lumora/logs/lumora.log`.

### Viewport controls

| Input | Action |
|---|---|
| W A S D (or arrows) / Q E | Fly / down, up while the pointer is over the viewport or after clicking it (Shift = faster) |
| Right mouse + drag | Look; the cursor stays locked in place (Right mouse + wheel = fly speed) |
| Middle mouse + drag | Pan |
| Wheel | Dolly |
| Left click | Select |
| 1 / 2 / 3 | Move / rotate / scale gizmo (Ctrl snaps: 0.25 m, 15°, 0.1×) |
| Esc | Clear selection |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| Ctrl+I | Import a glTF model |
| F | Frame selection |
| G | Toggle grid |
| Delete, Ctrl+D | Delete, duplicate |
| F5 / F6 / Shift+F5 | Play / Pause / Stop |
| F8 | Toggle the scene camera in play mode |

### Live collaboration

Several people can edit the same scene at once. One editor hosts the session; everyone else
joins it.

**Host (editor):** *Collaborate → Host Session…* (or the *Session* panel). Pick your name, a port
(default 7777, used for TCP and UDP) and optionally a password. The dialog lists this computer's
LAN addresses. From the command line:

```bash
./build/release/bin/Lumora --editor ~/LumoraProjects/Demo --host 7777 --user Ada
```

**Over the internet with [playit.gg](https://playit.gg):** run the playit.gg agent on the host
computer, add a tunnel of type **TCP+UDP** to local port 7777 and share the address playit.gg
shows (for example `bright-fox.at.ply.gg:23145`). Paste it into the host dialog to show it in the
*Session* panel with a Copy button. A TCP-only tunnel also works: camera positions then travel over
TCP instead of UDP.

**Join:** *Join Live Session…* in the Hub, *Collaborate → Join Session…* in the editor, or:

```bash
./build/release/bin/Lumora --join bright-fox.at.ply.gg:23145 --user Bob
```

A joiner works in a local copy of the host's project
(`~/.local/share/Lumora/Lumora/Sessions/<project>`). Models and textures the scene uses are
downloaded from the host when missing, and files a joiner imports are sent to the host and on
to everyone else. Saving writes each participant's own copy.

What is shared: every scene edit (entities, components, hierarchy, environment), undo and redo
included, plus each participant's camera and selection and a chat. Edits of different fields merge,
even on the same entity; when two people change the same field at once, the one the host
received last wins. While a session runs, *New Scene* and *Open Scene* are disabled.

The connection is not encrypted, and the password is only an entry check. Share session
addresses only with people you trust.

## Project format

```
MyGame/
  MyGame.lumora              project descriptor (JSON)
  Content/Scenes/Main.lscene scene (JSON, component data written through reflection)
  Content/Models/            imported glTF models (+ their buffers/images)
  Content/Textures/          imported images
  Saved/Thumbnail.png        written by the editor on save, shown in the Hub
```

Assets are referenced by project-relative keys: `Content/Models/Helmet.glb#0` (mesh 0),
`Content/Models/Helmet.glb#tex2` (image 2 inside the model) or `Content/Textures/Wood.png`.

Recent projects are stored in `~/.config/Lumora/recent_projects.json`. The Hub's
"Remove from List" never deletes project files.

## Repository layout

```
cmake/         build modules (compiler options, dependencies, shader compilation)
engine/        engine modules, no Qt: core, platform, input, assets, scene (ECS),
               project, renderer (Vulkan RHI + render graph + scene renderer)
runtime/       fixed-step simulation and gameplay systems
shaders/       HLSL sources compiled to SPIR-V at build time
application/   Qt layer: common (app context, Vulkan viewport, theme), hub, editor,
               player, collab (live sessions), main (CLI, window orchestration, headless)
tests/         unit, GPU and UI integration tests
docs/          architecture notes
```

The module boundaries, frame flow and the reasons behind key decisions are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Platform notes

- **Wayland:** the editor runs natively. Presentation uses MAILBOX, which never blocks the UI
  thread, and frames are paced to the display refresh. When the compositor reports the window
  as hidden (minimised, or the session locked), rendering pauses until the window is visible
  again. Wayland clients cannot warp the cursor, so right-mouse look locks it in place through
  the pointer-constraints and relative-pointer protocols (supported by KWin, Mutter and
  wlroots compositors). To force XWayland, run with `QT_QPA_PLATFORM=xcb`.
- **Windows:** the code has Windows paths (paths, Win32 surface extension, MSVC flags), but it
  has not been built or tested on Windows.

## Roadmap

Planned, in rough order: KTX2/Basis and HDR/EXR textures with an asset database and
background loading; skinned meshes and multiple materials per mesh; image-based lighting and virtual shadow maps; a render
thread with multithreaded command recording and a job system; clustered lighting; TAA and
temporal upscaling; GPU-driven culling, meshlets and mesh shaders (the capabilities are already
detected); hardware ray tracing (RT GI and reflections); Jolt physics; miniaudio; skeletal
animation; C++ gameplay modules with hot reload; Tracy integration; world streaming.

Only items in "What works today" are implemented.
