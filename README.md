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
| Editor | Embedded Vulkan viewport, fly camera, click picking with selection highlight, hierarchy (drag & drop re-parenting, rename, duplicate, delete, filter), reflection-driven inspector, content browser, console, GPU profiler, play/pause/stop, scene save/load, dockable and persisted layout |
| Renderer | Vulkan 1.4 instance/device with capability detection, VMA, dynamic rendering, synchronization2, render graph with automatic barriers, bindless descriptors (update-after-bind), timeline-semaphore uploads, persistent pipeline cache, GPU timestamp profiling, frames in flight, swapchain recreation, device-loss reporting |
| Rendering | HDR forward PBR (GGX / Smith / Schlick), directional + point lights, sun shadow map (camera-fitted, texel-snapped, hardware PCF), procedural sky with horizon haze and sun glow, exponential height fog (aerial perspective), physically based bloom, vignette, ACES tonemapping, procedural grid/checker surface patterns, hemispherical ambient, editor grid, CPU frustum culling |
| Scene | EnTT ECS, UUID identity, hierarchy, component reflection → JSON serialization + inspector |
| Runtime | Fixed-timestep simulation (deterministic), built-in `Rotator` behaviour |
| Tests | 73 GoogleTest cases: core, scene, serialization, projects, CLI, render-graph barrier planning, **GPU tests that render real frames and check pixels**, Hub/Editor UI integration tests |

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
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-svg-dev libvulkan-dev vulkan-validationlayers glslc mesa-vulkan-drivers
```

The first configure downloads GLM, EnTT, Vulkan Memory Allocator, nlohmann/json and GoogleTest
(pinned versions, SHA-256 verified).

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

Headless examples:

```bash
./build/release/bin/Lumora --headless --info
```

```bash
./build/release/bin/Lumora --headless --create ~/LumoraProjects/Demo --template showcase
```

```bash
./build/release/bin/Lumora --headless --project ~/LumoraProjects/Demo --validate --simulate 2 --render demo.png --width 1920 --height 1080
```

Global options: `--gpu <index>`, `--validation` / `--no-validation`, `--no-vsync`,
`--log-level <trace|debug|info|warn|error>`. Logs also go to `~/.local/state/Lumora/logs/lumora.log`.

### Viewport controls

| Input | Action |
|---|---|
| Right mouse + drag | Look |
| Right mouse + W A S D / Q E | Fly / down, up (Shift = faster, wheel = speed) |
| Middle mouse + drag | Pan |
| Wheel | Dolly |
| Left click | Select |
| F | Frame selection |
| G | Toggle grid |
| Delete, Ctrl+D | Delete, duplicate |
| F5 / F6 / Shift+F5 | Play / Pause / Stop |
| F8 | Toggle the scene camera in play mode |

## Project format

```
MyGame/
  MyGame.lumora              project descriptor (JSON)
  Content/Scenes/Main.lscene scene (JSON, component data written through reflection)
  Saved/Thumbnail.png        written by the editor on save, shown in the Hub
```

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
               player, main (CLI, window orchestration, headless)
tests/         unit, GPU and UI integration tests
docs/          architecture notes
```

The module boundaries, frame flow and the reasons behind key decisions are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Platform notes

- **Wayland:** the editor runs natively. Presentation uses MAILBOX, which never blocks the UI
  thread, and frames are paced to the display refresh. When the compositor reports the window
  as hidden (minimised, or the session locked), rendering pauses until the window is visible
  again. To force XWayland, run with `QT_QPA_PLATFORM=xcb`.
- **Windows:** the code has Windows paths (paths, Win32 surface extension, MSVC flags), but it
  has not been built or tested on Windows.

## Roadmap

Planned, in rough order: undo/redo and transform gizmos; glTF 2.0 / KTX2 / HDR asset import
with an asset database; image-based lighting and cascaded / virtual shadow maps; a render
thread with multithreaded command recording and a job system; clustered lighting; TAA and
temporal upscaling; GPU-driven culling, meshlets and mesh shaders (the capabilities are already
detected); hardware ray tracing (RT GI and reflections); Jolt physics; miniaudio; skeletal
animation; C++ gameplay modules with hot reload; Tracy integration; world streaming.

Only items in "What works today" are implemented.
