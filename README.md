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
| Application | Every module is its own shared library (`liblumora_core.so`, `…_renderer.so`, `…_editor.so`; `lumora_<module>.dll` on Windows) behind thin executables: `lumora-hub` (the Hub), `lumora-editor` (editor, player, headless tools of one version) and `Lumora` (development build: Hub → Editor → Hub in one process) |
| Hub | Hero banner rendered live by the engine, recent projects grid with thumbnails, search and sort, project creation from 3 templates with live-rendered previews (saved as the project thumbnail), project settings, GPU/engine info; **Editors page**: editor versions installed side by side from a signed release feed, projects opened in the version they were made with |
| Distribution | Packages per platform (editor and Hub ZIPs with the shared library, Qt libraries and plugins; system libraries never bundled), Ed25519-signed release feed, GitHub Actions workflow publishing to Cloudflare R2, installer that verifies size, SHA-256 and signature and refuses hostile archives; **delta updates** that download only the files (modules) that changed |
| Editor | Embedded Vulkan viewport, fly camera, click picking with selection highlight, move/rotate/scale gizmos (snapping, parent-space aware), undo/redo for every edit, hierarchy (drag & drop re-parenting, rename, duplicate, delete, filter), reflection-driven inspector with asset pickers, content browser (open scenes, add/drop-import models), console, GPU profiler, play/pause/stop, scene save/load, dockable and persisted layout |
| Collaboration | Live multi-user editing of one scene: host on a port (TCP + UDP), others join by LAN address or a playit.gg tunnel address; edits merge per field, collaborators' cameras and selections are drawn in their colour, chat, missing models/textures are transferred automatically, optional password |
| Renderer | Vulkan 1.4 instance/device with capability detection, VMA, dynamic rendering, synchronization2, render graph with automatic barriers and multithreaded recording of large passes (secondary command buffers), bindless descriptors (update-after-bind), timeline-semaphore uploads, persistent pipeline cache, GPU timestamp profiling, frames in flight, swapchain recreation, device-loss reporting |
| Rendering | HDR forward PBR (GGX / Smith / Schlick) with base colour, metallic-roughness, normal and emissive maps (mipmapped, anisotropic), directional lights and up to 4096 point lights (Forward+: binned into 32 px screen tiles on the CPU), cascaded sun shadows (4 texel-snapped cascades in one atlas, blended borders, smooth 7x7 PCF with receiver-plane bias; off-screen casters included), procedural sky with horizon haze and sun glow or **image-based lighting from an HDR environment** (equirectangular .hdr: GGX-prefiltered specular mips and SH9 irradiance computed on the job system), exponential height fog (aerial perspective), physically based bloom, vignette, ACES tonemapping, FXAA, procedural grid/checker surface patterns, hemispherical ambient, depth prepass and screen-space ambient occlusion (half resolution, depth-aware blur), editor grid and gizmo overlay pass, CPU frustum culling |
| Scene | EnTT ECS, UUID identity, hierarchy, component reflection → JSON serialization + inspector |
| Assets | glTF 2.0 import (.gltf/.glb: meshes with any number of materials, node hierarchy, PBR materials and textures, skins, animations), PNG/JPEG/TGA/BMP textures, portable project-relative asset references, import copies files into the project without overwriting |
| Physics | Jolt Physics rigid bodies (static, kinematic, dynamic) with box, sphere and capsule colliders, raycasts, contact events, and a character controller (walks, climbs steps, jumps), stepped on Jolt's thread pool during play |
| Runtime | Fixed-timestep simulation (deterministic), built-in `Rotator` behaviour, a job system for background asset loading and parallel loops |
| Audio | miniaudio: Audio Source component (WAV/FLAC/MP3, looping, volume, pitch, 3D attenuation and panning), listener at the camera, pause with play mode, scriptable |
| Animation | glTF node animation clips (linear, step, cubic-spline; translation, rotation, scale) played by an Animator component, scriptable; GPU skinning (4 joints per vertex) in shading, depth prepass and shadows |
| Prefabs | Entities with children saved as `.lprefab`, placed from the Content Browser (undoable) or spawned by scripts |
| Game export | *File > Export Game*: player runtime, content, built game library and a launcher in a standalone folder |
| Scripting | C++ behaviours in the project's `Scripts/` folder, compiled by the editor with CMake into a game library behind a C ABI and **hot-reloaded on save, also during play**; exceptions are caught per behaviour |
| Tests | 173 test cases (GoogleTest + CTest): core, scene, serialization, scene patches, projects, CLI, editor installer (feeds, signatures, hostile archives, installs from file feeds), glTF/texture import (incl. a locale regression), render-graph barrier planning, gizmo math, undo/redo, **GPU tests that render real frames and check pixels**, Hub/Editor UI integration tests, collaboration over real loopback sockets (convergence, assets, presence, hostile input) |

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

The executables are in `build/release/bin/` (`Lumora`, `lumora-editor`, `lumora-hub`, all thin
launchers) with compiled shaders in `bin/shaders/`; the modules are `build/release/lib/liblumora_*.so`.
`-DLUMORA_SHARED_MODULES=OFF` builds static modules instead.
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

**Sessions are encrypted.** The connection uses TLS (1.3 where available) and camera datagrams
are sealed with XChaCha20-Poly1305 under a per-participant key sent over TLS. The host has its own
certificate (created once, kept in `~/.local/share/Lumora/collab/`). The addresses in the
*Session* panel end with its fingerprint (`lumora://192.168.1.20:7777#ab12…`), and a guest
joining with such an address verifies it automatically. An address without one prompts once:
compare the fingerprint with the host's *Session* panel, and Lumora remembers it. If a host's
certificate ever changes for a remembered address, joining is refused. The password is
checked over the encrypted connection.

## Releases and installing editors

Users install only the **Hub**: an AppImage on Linux, an installer (`lumora-hub-setup.exe`, NSIS,
per user, no administrator rights) on Windows, or a portable ZIP. It installs editor versions next to each other
(`~/.local/share/Lumora/Editors/<version>/`, `%LOCALAPPDATA%\Lumora\Editors\<version>\` on Windows)
and opens each project in the version it was made with, as a separate process. The Hub's
*Editors* page lists installed versions, the releases in the feed and local builds (*Add Local
Build…* takes a `lumora-editor` you compiled yourself).

An editor package is a ZIP:

```
bin/lumora-editor        thin launcher (RPATH $ORIGIN/../lib)
bin/qt.conf, bin/shaders/*.spv
lib/liblumora_*.so       one library per module (core, scene, renderer, ..., editor, hub, shell)
lib/libQt6*.so*, libicu* Qt and its ICU only - glibc, libstdc++, X11/Wayland, the Vulkan loader
                         and OpenSSL always come from the system
plugins/                 Qt platform (xcb, Wayland), TLS, image-format and icon plugins
```

On Windows everything sits in `bin/` (DLLs next to the executable, deployed by `windeployqt`).

The release feed is `editor/manifest.json` in the bucket: every release with, per platform,
the package URL, size and SHA-256. `manifest.json.sig` is its Ed25519 signature. A Hub built
with `-DLUMORA_RELEASE_PUBLIC_KEY=<key>` refuses feeds not signed by that key, and every
download must match the size and SHA-256 the signed feed lists. Packages unpack into a staging
folder and only then move into place. Archives with absolute paths, `..` or links pointing
outside are refused.

**Updates download only what changed.** Next to each ZIP, the feed lists the package's files
(`<zip>.files.json`: path, size, SHA-256, mode), and every file is stored once in
`editor/blobs/<sha256>`. When a version is already installed, the Hub copies the files it
already has (matched by hash) and downloads only the others - usually the few modules a release
touched. If anything on that path fails, it downloads the full ZIP instead.

### Publishing (GitHub Actions → Cloudflare R2)

One-time setup:

1. In Cloudflare R2, enable the bucket's **Public Development URL** (or attach a custom domain).
   Create an **R2 API token** with *Object Read & Write* for that bucket only.
2. Generate the signing key. The secret half goes to a file and is never printed; store it
   with `gh` (which does not display it) and keep the file outside the repository or delete it:

   ```bash
   python3 scripts/release/manifest.py keygen --secret-file ~/lumora-signing.key
   ```

   ```bash
   gh secret set LUMORA_SIGNING_KEY --env release < ~/lumora-signing.key
   ```

   The printed `LUMORA_RELEASE_PUBLIC_KEY` is public: it becomes a repository variable.

3. In GitHub → *Settings → Secrets and variables → Actions*, add:
   - secrets: `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, `LUMORA_SIGNING_KEY`
   - variables: `R2_ENDPOINT` (`https://<account id>.r2.cloudflarestorage.com`), `R2_BUCKET`,
     `LUMORA_RELEASE_PUBLIC_KEY`, `LUMORA_RELEASE_FEED`
     (`https://<public url>/editor/manifest.json`)
   - an environment named `release` (add required reviewers there if publishing should need approval)

Each release: set `VERSION` in `project()` in `CMakeLists.txt`, commit, then push a matching tag.
Its suffix picks the **channel**: `v0.2.0` is stable, `v0.2.0-beta.1` and `v0.2.0-rc.1` are beta,
`v0.2.0-alpha.3` is alpha. The suffix becomes part of the version the binaries report
(`-DLUMORA_VERSION_SUFFIX=beta.1`) and of the projects they save.

```bash
git tag v0.2.0-beta.1 && git push origin v0.2.0-beta.1
```

In the Hub, *Editors → Show* chooses what is offered: stable releases (default), stable and
beta, or everything including alpha. *Install Latest* installs the newest one of those.

The workflow `.github/workflows/release.yml` builds and tests on Linux and Windows. It packages
the editor and the Hub, then installs the fresh editor package with the Hub's own installer. If
that works, it uploads `editor/<version>/…`, the new blobs and `hub/<version>/…` (plus
`hub/latest/lumora-hub-<platform>.zip`, the newest Hub on any channel) and updates and signs the feed.
*Run workflow* builds without publishing unless you tick *publish*. It takes a channel and
an optional suffix; alpha and beta default to `<channel>.<run number>`.

Packaging locally:

```bash
cmake --install build/release --component editor --prefix dist/editor
```

```bash
python3 scripts/release/package.py --install-dir dist/editor --product editor --version 0.1.0 --platform linux-x64 --out out
```

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
runtime/       fixed-step simulation, physics, gameplay scripting (game library loading)
sdk/           scripting SDK headers game libraries compile against (shipped in include/)
shaders/       HLSL sources compiled to SPIR-V at build time
application/   Qt layer: common (app context, Vulkan viewport, theme), hub, editor,
               player, collab (live sessions), launcher (editor installer),
               main (entry point, CLI, window orchestration, headless)
scripts/       release tooling: package.py (ZIP), manifest.py (feed, signing keys)
.github/       release workflow (build, test, package, publish to R2)
tests/         unit, GPU and UI integration tests
docs/          user guide and architecture notes
site/          project website (GitHub Pages)
```

A step-by-step user guide is in [docs/GUIDE.md](docs/GUIDE.md) and on the
[website](https://lumoraengine.xyz/docs.html). The module boundaries, frame flow and the reasons behind key decisions are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Platform notes

- **Wayland:** the editor runs natively. Presentation uses MAILBOX, which never blocks the UI
  thread, and frames are paced to the display refresh. When the compositor reports the window
  as hidden (minimised, or the session locked), rendering pauses until the window is visible
  again. Wayland clients cannot warp the cursor, so right-mouse look locks it in place through
  the pointer-constraints and relative-pointer protocols (supported by KWin, Mutter and
  wlroots compositors). To force XWayland, run with `QT_QPA_PLATFORM=xcb`.
- **Windows:** the code has Windows paths (paths, Win32 surface extension, MSVC flags, DLL
  export of the entry point, `windeployqt` packaging) and the release workflow builds it, but no
  Windows build has been run or tested yet.
- **Linux packages** are built on Ubuntu 24.04 (glibc 2.39). Older distributions need a newer
  glibc.

## Roadmap

Planned, in rough order: KTX2/Basis and HDR/EXR textures with an asset database and
background loading; virtual shadow maps; a render
thread; TAA and
temporal upscaling; GPU-driven culling, meshlets and mesh shaders (the capabilities are already
detected); hardware ray tracing (RT GI and reflections); Tracy integration; world streaming.

Only items in "What works today" are implemented.

## License

Lumora is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE).
Third-party components and their licences are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
Contributions follow [CONTRIBUTING.md](CONTRIBUTING.md).
