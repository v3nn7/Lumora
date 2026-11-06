# Lumora architecture

This document describes the architecture as it is implemented now. Planned systems are named only
where the current design leaves room for them.

## Module graph

```
         Lumora · lumora-editor · lumora-hub   (thin executables: main() → runLumora(flavor))
                                 │
                           lumora_shell  (entry point, window orchestration, headless mode)
                ┌────────────────┼────────────────┐
           lumora_hub       lumora_editor     lumora_player
                ├── lumora_launcher  (editor installer: release feed, signatures, ZIP)
                │                ├── lumora_collab  (live sessions: Qt Network + scene patches)
                └────────────────┼────────────────┘
                         lumora_appcommon  (AppContext, VulkanViewport, theme, snapshots)
                                 │                         Qt 6 boundary ▲
 ────────────────────────────────┼──────────────────────────────────────────
     lumora_runtime ── lumora_project ── lumora_renderer      lumora_cli
                 └──────────┬────────────────┘                    │
                       lumora_scene (EnTT, reflection, serializer, camera, picking)
                  ┌─────────┼──────────┐
            lumora_assets  lumora_input  lumora_core (log, errors, uuid, fs, platform paths)
```

- Nothing under `engine/` or `runtime/` includes Qt. The editor is a client of the engine, not
  part of it. The same renderer runs headless.
- Each module is a shared library (`liblumora_<module>.so`, `lumora_<module>.dll`) with an
  explicit dependency list (`engine/CMakeLists.txt`, `application/CMakeLists.txt`,
  `cmake/LumoraModules.cmake`), linked with `--no-undefined` on Linux. A release that touches
  one module changes one file, which delta updates exploit. On Windows functions are exported
  automatically (`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`); QObject classes, whose `staticMetaObject`
  other modules reference, carry `LUMORA_<MODULE>_API`. Singletons (`Logger::instance`,
  `JobSystem::global`, `ComponentRegistry::builtin`) are defined out of line, so each exists
  once; third-party static libraries (Jolt, miniz, Monocypher) are linked into exactly one
  module. `LUMORA_SHARED_MODULES=OFF` builds static modules.

## Conventions

| Topic | Convention |
|---|---|
| World space | Right-handed, +Y up, cameras look down −Z, metres |
| Rotation storage | Euler degrees, applied yaw (Y) · pitch (X) · roll (Z). Lossless for editing and serialization; quaternions are used for incremental rotation (`runtime/Simulation.cpp`) |
| Projection | Infinite, **reversed-Z** (near → 1, far → 0, depth test `GREATER`), Y flipped in the projection matrix for Vulkan clip space |
| Winding | Counter-clockwise front faces (tested per primitive) |
| Colour | HDR linear `R16G16B16A16_SFLOAT` scene colour; sRGB swapchain when available, otherwise the encode happens in the tonemap shader |
| Errors | `std::expected` (`lumora::Result`) for recoverable failures (I/O, parsing). Exceptions for unrecoverable initialisation failures (`rhi::UnsupportedHardwareError`, `rhi::VulkanError`) and device loss (`rhi::DeviceLostError`), which the viewport turns into a visible message |
| Ownership | RAII everywhere: NVRHI handles (`nvrhi::BufferHandle`, `TextureHandle`, ...) for GPU resources and `rhi::DeviceHandle<T, vkDestroyT>` for the few raw Vulkan handles left (semaphores). No raw `new`/`delete` in engine code |

## Renderer

### Layers

1. **RHI (`engine/renderer/vulkan`)**: `Instance`, `Device` (selection plus `GpuCapabilities`),
   `Swapchain`, `Buffer` / `Image`, `ShaderLibrary`, `PipelineCache`, graphics pipeline builder,
   `BindlessTable` and `UploadQueue`.
2. **Frame infrastructure**: `RenderGraph`, `TransientImagePool`, `GpuProfiler`, `FrameResources`.
3. **`RenderContext`**: process-wide GPU state shared by every viewport. It holds the device,
   allocator, bindless table, pipeline layout, samplers, shaders, pipeline cache and built-in meshes.
4. **`Renderer`**: frames in flight, recording, submission, presentation, capture.
5. **`SceneRenderer`**: turns a `Scene` and a `CameraView` into render-graph passes.

### Device requirements

`enumerateDevices` evaluates every GPU and records a rejection reason for each one that fails.
Required: API version ≥ 1.4, a graphics+compute queue, `dynamicRendering`, `synchronization2`,
`timelineSemaphore`, update-after-bind descriptor indexing (`runtimeDescriptorArray`,
`partiallyBound`, `sampledImageUpdateAfterBind`, `updateUnusedWhilePending`) and
`shaderDemoteToHelperInvocation`. Ray tracing, ray query, mesh shaders, timestamps and memory
budget are detected and reported, but nothing uses ray tracing or mesh shaders yet.
`VK_KHR_swapchain` is enabled only when a context presents (headless contexts have no surface
extensions).

### Descriptor model

- **Set 0 (per frame slot):** `FrameConstants` UBO (camera, environment, lights, cascade transforms)
  and a `GpuObject[]` storage buffer (world matrix, normal matrix, material).
- **Set 1 (global, bindless):** `Texture2D[]` and `SamplerState[]`, with update-after-bind and
  partially bound. The sampler array is also declared as `SamplerComparisonState[]` (descriptor
  aliasing) for shadow PCF.
- **Push constants (16 bytes):** object index, bindless indices, one float, flags.

C++ mirrors of the shader structures (`renderer/GpuTypes.hpp`) use only 16-byte vectors and
4×4 matrices, so std140, std430 and C++ layout are identical. `static_assert`s pin the offsets,
which were checked against the SPIR-V decorations glslc emits.

### Frame flow

```
Renderer::render(target, scene, camera, settings)
  ├─ recreate swapchain if resized / suboptimal
  ├─ wait for this slot's fence → resolve GPU timestamps → recycle transient images
  ├─ acquire image (20 ms timeout: Timeout → skip frame, UI thread never stalls)
  ├─ record:  RenderGraph
  │     SunShadow (4 cascades → atlas) → Sky → Opaque PBR (samples atlas) → Grid → Tonemap → [Readback]
  │     graph: realise transients from the slot's pool, plan barriers, begin/end dynamic rendering,
  │            debug labels, timestamps per pass
  ├─ submit2: wait acquire (COLOR_ATTACHMENT_OUTPUT) + upload timeline value; signal per-image
  │           present semaphore; fence
  └─ present (OUT_OF_DATE / SUBOPTIMAL → recreate next frame)
```

### Sun shadows

Four cascades share one depth atlas (2×2 tiles, `shadowResolution` each, clamped to the device's
`maxImageDimension2D`). Cascade *i* covers the view frustum from the near plane to split *i*
(practical scheme, λ = 0.8 between logarithmic and uniform), so the cascades are nested and every
visible point within the shadow distance has one. Each cascade is a bounding-sphere ortho box
(rotation-invariant size) snapped to whole texels, which keeps edges from shimmering as the camera
moves; its depth range is centred on the slice so that depth 0.5 is the centre.

Casters are culled per cascade against the box's side planes, never against the camera frustum,
so objects outside the view still cast into it. The light-facing plane does not cull.

The fragment shader picks the finest cascade whose tile keeps the filter footprint inside it,
blends into the next cascade over the outer 20 % (in x, y and depth) and fades shadows out over
the last 15 % of the shadow distance. Filtering is Castaño's optimised 7×7 PCF: 16 bilinear
comparisons whose weights form a smooth tent, with no noise and therefore no need for TAA.
Acne is handled by a slope-scaled raster bias in the shadow pass, a normal offset sized to the
cascade texel (larger at grazing angles) and a receiver-plane depth bias. That bias is computed
from world-position derivatives taken in uniform control flow and mapped through the affine
cascade transform. `RenderSettings::debugCascades` tints surfaces red/green/blue/yellow by cascade.

`Renderer::capture` runs the same path into an offscreen `R8G8B8A8_SRGB` image followed by a
readback pass. The headless renderer, Hub template previews, project thumbnails and the GPU
tests all use it.

### Render graph

Passes declare their accesses: `writeColor`, `writeDepth`, `readDepth`, `sample`, `copyFrom`,
`copyTo`. Each usage maps to one `(layout, stages, access)` state. `planBarriers()` is a pure
function and is unit-tested. It emits a barrier on a layout change or a write hazard, skips
read-after-read, and limits write-after-read to an execution dependency. Transient images
start each frame undefined (`discard`). Imported images carry an initial state; for the
swapchain image that is `COLOR_ATTACHMENT_OUTPUT`, so the first barrier chains with the acquire
semaphore wait. Imported images can also carry a final usage (`Present`).

Transient images come from a per-frame-slot pool. An image is reused only after that slot's fence
has signalled, which removes cross-frame hazards without extra barriers. Sampled transients are
registered in the bindless table automatically.

Compute passes (`addComputePass`) declare their storage images with `writeStorage`; the graph
binds them in order as set 2 (`RenderGraph::kStorageSlots` slots, unused ones filled with a 1x1
stand-in) and `PassContext::storageBindings()` / `frameBindings()` hand the sets to the pass.
A transient used that way is created with `storage = true` (unordered access). Shaders name the
storage format with glslang's `[[spv::format_rgba16f]]` style attribute.

Not yet implemented: pass culling, resource aliasing and async-compute scheduling.

### Uploads

`UploadQueue` records each copy into its own command buffer and signals a timeline semaphore.
Frame submissions wait on the latest value on the GPU, so the CPU does not wait for uploads.
Staging memory is reclaimed once the timeline passes the upload's value. Uploads currently use
the graphics queue. A dedicated transfer queue (with ownership transfers) belongs with asset
streaming.

### Presentation and threading

Rendering currently happens on the Qt UI thread. Three decisions keep that responsive:

1. **Present policy:** vsync uses MAILBOX when it is available, because MAILBOX never blocks the
   caller. On Wayland a FIFO `vkQueuePresentKHR` waits for compositor frame callbacks, and the
   compositor withholds those while the window is hidden (for example when the session is locked).
   That caused a real deadlock during development. FIFO is only a fallback.
2. **Pacing:** the viewport drives frames with a precise `QTimer` at the screen refresh rate.
   It does not use `QWindow::requestUpdate()`, because on Wayland Qt ties update requests to frame
   callbacks of surfaces that Qt itself commits, and the driver commits Vulkan surfaces.
3. **Visibility:** rendering stops when Qt reports the window as unexposed and resumes on the next
   expose event.

Planned: a dedicated render thread that consumes an immutable render snapshot of the scene, with
multithreaded command recording.

## Scene and ECS

- `Scene` wraps an `entt::registry` and adds a UUID index plus explicit root/child ordering, so
  hierarchy display and serialization are deterministic: serialising a loaded scene again gives
  identical output (tested).
- Core components: `Id`, `Name`, `Transform`, `WorldTransform` (cache), `Hierarchy`,
  `MeshRenderer`, `Light`, `Camera`, `Rotator`.
- **Scene patches** (`ScenePatch`): the difference between two scene states, as small as possible.
  New entities are full records (with parent and sibling index). Changed entities are partial
  records carrying only the new name, a move (parent + index), the changed fields of each
  component, and ids of added or removed components. A reorder moves only the siblings outside
  the longest run that is already in order, so deleting or inserting a sibling never rewrites
  its neighbours. Applying a patch ignores edits of entities or components that no longer exist,
  so a deletion wins over edits. Undo/redo and collaboration both use patches.
- **Reflection** (`ComponentRegistry`): each component describes its fields with typed accessors.
  That one description drives JSON serialization, the inspector UI and the "Add Component" menu.
  Adding a reflected component therefore requires no serializer or editor changes. Unknown
  components and fields are skipped with a warning (forward compatibility).

## Assets

- `importGltf` (cgltf) turns a .gltf/.glb into `ImportedModel`: meshes (triangle primitives
  merged, missing normals/tangents generated), node hierarchy (TRS or decomposed matrices),
  metallic-roughness materials and still-encoded images. `instantiateModel` mirrors the node
  hierarchy as entities.
- `AssetLibrary` (per project) resolves portable keys, `<file>#<mesh>`, `<file>#tex<image>` and
  plain image paths. Keys must be relative and stay inside the project. It caches models,
  decoded textures and failures, so a broken reference is reported once.
- The renderer's `MeshCache` and `TextureCache` upload on first use (non-blocking timeline
  uploads; textures get a full blit-generated mip chain, sRGB views for colour maps) and key
  GPU data by absolute path, so projects never collide.
- JSON numbers are parsed locale-independently. Qt switches `LC_NUMERIC` to the user's locale,
  and cgltf's default `atof` read `0.01` as `0` under pl_PL; a regression test covers this.

## Runtime

`Simulation` advances gameplay systems at a fixed timestep (60 Hz by default), with a bounded
number of steps per frame to avoid a spiral of death. Given the same number of steps the result
is deterministic (tested). Editor play mode clones the edited scene through the serializer,
simulates the copy and discards it on Stop, so play-mode changes never reach the saved scene.

## Application layer

- `AppContext` holds services shared by every window. It initialises Vulkan lazily: the Hub
  works without a GPU, and if initialisation fails the editor viewport shows the exact reason.
  It also adopts the engine's `VkInstance` into a `QVulkanInstance`, so Qt can create
  platform surfaces.
- `VulkanViewport` is a `QWindow` (Vulkan surface type) embedded with
  `QWidget::createWindowContainer`. It owns the swapchain and releases it on
  `SurfaceAboutToBeDestroyed`, before Qt destroys the surface. Input is translated to the
  engine's `InputState`.
  - **Keyboard:** on Wayland the embedded (subsurface) window never receives keyboard focus, so
    the viewport installs an application event filter and takes keys that reach its own
    top-level window while it is hovered, was clicked last, or is looking. Text fields keep their
    keys. WASDQE/arrows/Space are consumed for the camera; other keys go to the client's
    shortcut handler.
  - **Mouse look:** X11 recentres the cursor with `QCursor::setPos`. Wayland forbids warping, so
    `WaylandPointerLock` locks the pointer (pointer-constraints) and reads relative motion
    (relative-pointer). Qt does not expose these protocols: the client code is generated with
    `wayland-scanner` from pinned XML (`cmake/LumoraWayland.cmake`). Its objects, including a
    second `wl_pointer` that tracks which surface has the pointer, live on a private event queue
    dispatched only on the UI thread, so Qt's own Wayland objects are untouched.
- `ViewportClient` is the seam between a viewport and its owner (`EditorViewportClient`,
  `PlayerWindow`).
- `EditorContext` holds the active scene, the selection, the undo stack, mutation entry points
  and change signals. Panels never modify the scene behind each other's backs.
- **Undo/redo** (`QUndoStack`): structural edits record the scene before and after and apply the
  `ScenePatch` between them in place, so the `Scene` object and every pointer to it stay valid,
  UUIDs survive, and only what the edit changed is reverted (collaborators' concurrent changes
  stay). Field and environment edits are typed commands that merge bursts
  (scrubbing, sliders) into one step. Gizmo drags record one transform command on release.
  The dirty flag follows the stack's clean state; play-mode edits are never recorded.
- **Gizmos**: `TransformGizmo` picks handles in screen space and constrains drags to the picked
  axis (world-space move/rotate, local-space scale, parent-space aware, Ctrl snapping). Its
  geometry goes through the renderer's overlay pass (unlit, alpha-blended, after tone mapping).
- `Application` owns the top-level windows and performs the Hub ↔ Editor transitions. On
  teardown it flushes deferred deletions before the `AppContext` is destroyed (a use-after-free
  that ASan found).

## Distribution

- **Flavours.** `runLumora(argc, argv, flavor)` is the whole application. `Lumora` (all-in-one,
  for development) and `lumora-editor` behave the same. `lumora-hub` accepts only Hub mode and
  sets `Application::setExternalEditors`, so opening a project starts the installed editor
  matching the project's `engineVersion` as a separate process (`--editor <project>`). If
  none matches it asks before using the newest one, and if nothing is installed it sends the
  user to the Editors page. Joining a session from that Hub starts an editor with `--join`; the
  password goes through `LUMORA_SESSION_PASSWORD`, not the command line.
- **Packages.** `cmake --install --component editor|hub` lays out `bin/` (launcher, `qt.conf`,
  shaders) and `lib/` (`liblumora`). On Linux `cmake/LumoraDeployLinux.cmake.in` adds an
  allow-list of Qt plugins (xcb, Wayland, TLS, SVG/JPEG/GIF/ICO, input contexts) and, through
  `file(GET_RUNTIME_DEPENDENCIES)`, exactly the `libQt6*` and `libicu*` libraries they need,
  nothing from the system. On Windows Qt's deployment API runs `windeployqt`. The executables
  are created with `qt_add_executable` and link the Qt modules so the deployment knows them.
  RPATHs are `$ORIGIN/../lib` (executables) and `$ORIGIN` (library).
- **Installer** (`application/launcher`). `ReleaseManifest` parses the feed (versions are
  validated because they name folders; relative URLs resolve against the feed).
  `verifyEd25519` (Monocypher) checks the detached signature against the key compiled in with
  `LUMORA_RELEASE_PUBLIC_KEY`. A test pins interoperability with the Python release tooling.
  `EditorInstaller` streams the download to disk while hashing it, rejects anything larger than
  announced or not matching the signed SHA-256, extracts on a worker thread into
  `.staging-<version>`, writes `lumora-install.json` and renames the folder into place.
  `extractZip` (miniz, read through `QFile` callbacks) validates every name before writing:
  no absolute paths, drive letters, backslashes or `..`. Link targets must be plain relative
  paths, sizes and entry counts are bounded, and Unix modes are restored.
  `EditorRegistry` merges installed versions with local builds registered in `QSettings`
  (version read from `--version`).
- **Delta updates.** `package.py` also writes the package's file list and copies each file to
  `blobs/<sha256>`; the signed feed carries the list's URL, size and hash. With a version
  installed, `EditorInstaller` downloads and verifies the list, copies every file whose hash an
  installed version has (from that version's `lumora-files.json`, written at install time) on a
  worker thread, downloads the remaining blobs one by one (each checked against its hash), then
  recreates links and modes and finalises like a full install. Any failure falls back to the
  full ZIP.
- **Release pipeline.** `scripts/release/package.py` writes deterministic ZIPs (fixed
  timestamps, Unix modes, symlinks kept) plus a size/SHA-256 fragment. `manifest.py` merges
  releases into the feed, signs and verifies it, and generates keys. The workflow builds and
  tests both platforms, then checks each editor package by installing it through
  `lumora_launcher_tests` (`LUMORA_TEST_FEED`) from a feed signed with a throwaway key. Only then
  does it upload to R2 (S3 API) and replace the feed with `Cache-Control: no-cache`.

## Live collaboration

`lumora_collab` (Qt Network, no engine changes beyond `ScenePatch`) has a server, a client, the
scene synchronisation and the editor-facing dialogs and panel.

**Transport.** The host listens on one port for TCP and UDP. TCP is a stream of frames
`u32 length | u8 kind | payload`: kind 0 is a JSON object with a `"t"` type, kind 1 is an asset
chunk (path, offset, total size, bytes). UDP datagrams (`"LMRU" | JSON`) carry only presence:
camera position, view direction and selection. A client switches to UDP once the host has
answered its UDP hello within the last 5 s, and otherwise sends presence over TCP and tells the
host to relay others' presence over TCP. A TCP-only tunnel therefore loses nothing but latency.

**Ordering.** The server keeps the authoritative scene (a `Scene` built from the host's document)
and applies every patch in arrival order, then broadcasts it with a sequence number to everyone,
the author included. The echo is the acknowledgement. Each patch from a client is applied as a
whole, and the last writer per field wins.

**SceneSync.** A client finds local edits by diffing its scene against the state last exchanged
with the host, about 15 times per second. That covers every editing path (inspector, gizmo
drags, undo, imports) without hooks. Each sent patch is recorded as fine-grained pending keys
(entity × field, name, move, component added/removed, full record, removal). When someone else's
patch arrives, local edits are sent first, and then the parts of that patch that a pending key
covers are dropped. Our patch is later in the host's order, so the host ends up with our value
too, and everyone converges without operational transforms. Our own echo releases the keys.

**Drift repair.** Every 3 s the host broadcasts a digest (SHA-1 of the scene JSON) with its
sequence number. A client that has applied that sequence and has nothing in flight compares
digests; on a mismatch it asks for a snapshot and replaces its scene with it. That covers cases
the merge rule does not (for example sibling order after concurrent inserts).

**Assets.** The host's project folder is the shared store. Clients request files that the scene
references but their project lacks (and, for `.gltf`, the buffers and images it names). When a
client's patch refers to a file the host lacks, the host fetches it from that client, then
announces it, and clients that were told "missing" ask again. Transfers stream in 256 KiB chunks
with back-pressure, are written to `<path>.part` and renamed only when complete. Existing files
are never overwritten. When files arrive the editor clears cached load failures
(`AssetLibrary`, `MeshCache`, `TextureCache`), so they appear without a reload.

**Limits and checks.** Before a client is admitted, JSON frames are limited to 64 KiB; after
that, 64 MiB for messages and 256 KiB per chunk. JSON nesting deeper than 64 is rejected before
parsing. Asset paths must be relative, inside `Content/`, with no `..`, empty, dot or `.part`
segments. Asset files are limited to 1 GiB, chat to 500 characters and names to 32. A peer has
10 s to say hello, sessions hold up to 16 clients, and a password, if set, is checked on
admission. Patches from the network are validated like scene files.

**Encryption.** The TCP stream is TLS (`QSslServer`/`QSslSocket`, TLS 1.2 or later). The host's
identity (`HostIdentity`) is an ECDSA P-256 key (micro-ecc) in a self-signed X.509 certificate
that Lumora encodes in DER itself, because Qt cannot create certificates and no OpenSSL headers
are needed. It is created once per user and reused, so a guest's trust carries over. Guests
verify the certificate by SHA-256 fingerprint, not by a CA. The fingerprint is pinned in the
shared address (`lumora://host:port#<sha256>`), else taken from `KnownHosts` (remembered after
the user confirmed it once). An unknown host is reported (`untrustedHost`) and not joined, and
a mismatch is a hard failure. UDP presence datagrams are `"LMRE" | id | nonce | MAC |
XChaCha20-Poly1305(JSON)` (Monocypher), keyed per participant with a random key sent in the
welcome message, with the header authenticated as associated data. The host drops datagrams
that do not open under the claimed participant's key.

**Editor integration.** The host's editor runs the server in-process and joins it over loopback
like everyone else. A joiner connects first. The `joined` signal is handled synchronously: the
Application opens (or creates) a local session project in `AppLocalDataLocation/Sessions/<project>`
and hands the client to that editor before any later message is read. Remote patches go through
`EditorContext::applyPatch`, or straight to the edit scene during play mode. Collaborators'
cameras and selections are drawn by `EditorViewportClient` through the overlay pass in their
colour.

## Testing

| Suite | What it proves |
|---|---|
| Core / scene / serialization / project / CLI | Logic, file formats, error paths (corrupt or missing files, cycles, duplicate UUIDs) |
| Primitives | Index validity, unit normals, **CCW winding per triangle**, bounds |
| RenderGraph | Barrier planning for typical frames and hazard types |
| Renderer (GPU) | Renders real frames offscreen and asserts on pixels: a lit cube against sky, back-face culling, frustum culling counts. Skipped when no Vulkan 1.4 device exists |
| UI (offscreen QPA) | Hub → Editor → Hub, hierarchy editing, reparent + save round trip, play-mode isolation, reflection-built inspector, two editors sharing a live session |
| Scene patches | Every kind of edit round-trips, partial records, reorders, concurrent field merges, deletion wins, malformed input |
| Collaboration (loopback sockets) | Framing under arbitrary splits, hostile frames, path confinement, address parsing, joining, convergence under concurrent edits, drift repair from the digest, presence over UDP, chat, asset download and guest upload, path traversal, wrong password and old protocol versions, host shutdown |

Useful environment variables: `LUMORA_TEST_VALIDATION=1` (validation in GPU tests), `LUMORA_GPU=n`,
`LUMORA_SHADER_DIR`, and `LUMORA_UI_SNAPSHOT=/path/prefix`, which writes PNGs of all windows
with the Vulkan viewports composited in. With `LUMORA_UI_SNAPSHOT_QUIT=1` the app then leaves any
live session and quits without prompts. Two such processes, one started with
`--editor <p> --host <port>` and one with `--join 127.0.0.1:<port>`, make a scripted two-process
session test.

## Key decisions

| Decision | Reason |
|---|---|
| Per-entity scene patches for structural undo | Only what an edit changed is reverted, so undo stays correct while others edit the same scene; the same patches are the collaboration wire format |
| Host-ordered patches with pending-key filtering for collaboration | Converges without OT/CRDT machinery: the host's order decides, a client skips only what its own in-flight patch overrides, and a periodic digest repairs anything the rule misses |
| HLSL compiled by `glslc` (shaderc) | Ships with the Vulkan SDK and Debian. Slang is not packaged here; shaders are plain HLSL, so a later switch to `slangc` only changes `cmake/LumoraShaders.cmake`. glslang quirks are documented in `shaders/common.hlsli` (`vk::push_constant` only works on `ConstantBuffer<T>`; there is no `NonUniformResourceIndex`) |
| Vulkan C API with thin RAII wrappers | Full control and fast compile times; Vulkan-Hpp RAII adds a lot of compile time |
| One process for Hub and Editor | A requirement. It also lets them share the Vulkan context, log history and recent-project state |
| Lazy Vulkan initialisation | The Hub stays usable on machines without Vulkan 1.4, and failures are reported where the user looks |
| Euler storage for rotation | Lossless round trip through the inspector and files. Gizmos will introduce quaternion editing internally |
