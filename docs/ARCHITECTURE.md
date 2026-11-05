# Lumora architecture

This document describes the architecture as it is implemented now. Planned systems are named only
where the current design leaves room for them.

## Module graph

```
                      Lumora (executable: main.cpp)
                                 │
                           lumora_shell  (window orchestration, headless mode)
                ┌────────────────┼────────────────┐
           lumora_hub       lumora_editor     lumora_player
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
- Each module is a static library with an explicit dependency list
  (`engine/CMakeLists.txt`, `application/CMakeLists.txt`).

## Conventions

| Topic | Convention |
|---|---|
| World space | Right-handed, +Y up, cameras look down −Z, metres |
| Rotation storage | Euler degrees, applied yaw (Y) · pitch (X) · roll (Z). Lossless for editing and serialization; quaternions are used for incremental rotation (`runtime/Simulation.cpp`) |
| Projection | Infinite, **reversed-Z** (near → 1, far → 0, depth test `GREATER`), Y flipped in the projection matrix for Vulkan clip space |
| Winding | Counter-clockwise front faces (tested per primitive) |
| Colour | HDR linear `R16G16B16A16_SFLOAT` scene colour; sRGB swapchain when available, otherwise the encode happens in the tonemap shader |
| Errors | `std::expected` (`lumora::Result`) for recoverable failures (I/O, parsing). Exceptions for unrecoverable initialisation failures (`rhi::UnsupportedHardwareError`, `rhi::VulkanError`) and device loss (`rhi::DeviceLostError`), which the viewport turns into a visible message |
| Ownership | RAII everywhere: `rhi::Buffer` / `rhi::Image` (VMA) and `rhi::DeviceHandle<T, vkDestroyT>` for simple handles. No raw `new`/`delete` in engine code |

## Renderer

### Layers

1. **RHI (`engine/renderer/vulkan`)**: `Instance`, `Device` (selection plus `DeviceCapabilities`),
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

- **Set 0 (per frame slot):** `FrameConstants` UBO (camera, environment, lights, shadow transform)
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
  │     SunShadow (depth) → Sky → Opaque PBR (samples shadow map) → Grid → Tonemap → [Readback]
  │     graph: realise transients from the slot's pool, plan barriers, begin/end dynamic rendering,
  │            debug labels, timestamps per pass
  ├─ submit2: wait acquire (COLOR_ATTACHMENT_OUTPUT) + upload timeline value; signal per-image
  │           present semaphore; fence
  └─ present (OUT_OF_DATE / SUBOPTIMAL → recreate next frame)
```

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
- **Reflection** (`ComponentRegistry`): each component describes its fields with typed accessors.
  That one description drives JSON serialization, the inspector UI and the "Add Component" menu.
  Adding a reflected component therefore requires no serializer or editor changes. Unknown
  components and fields are skipped with a warning (forward compatibility).

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
- `ViewportClient` is the seam between a viewport and its owner (`EditorViewportClient`,
  `PlayerWindow`).
- `EditorContext` holds the active scene, the selection, mutation entry points and change
  signals. Panels never modify the scene behind each other's backs.
- `Application` owns the top-level windows and performs the Hub ↔ Editor transitions. On
  teardown it flushes deferred deletions before the `AppContext` is destroyed (a use-after-free
  that ASan found).

## Testing

| Suite | What it proves |
|---|---|
| Core / scene / serialization / project / CLI | Logic, file formats, error paths (corrupt or missing files, cycles, duplicate UUIDs) |
| Primitives | Index validity, unit normals, **CCW winding per triangle**, bounds |
| RenderGraph | Barrier planning for typical frames and hazard types |
| Renderer (GPU) | Renders real frames offscreen and asserts on pixels: a lit cube against sky, back-face culling, frustum culling counts. Skipped when no Vulkan 1.4 device exists |
| UI (offscreen QPA) | Hub → Editor → Hub, hierarchy editing, reparent + save round trip, play-mode isolation, reflection-built inspector |

Useful environment variables: `LUMORA_TEST_VALIDATION=1` (validation in GPU tests), `LUMORA_GPU=n`,
`LUMORA_SHADER_DIR`, and `LUMORA_UI_SNAPSHOT=/path/prefix`, which writes PNGs of all windows
with the Vulkan viewports composited in.

## Key decisions

| Decision | Reason |
|---|---|
| HLSL compiled by `glslc` (shaderc) | Ships with the Vulkan SDK and Debian. Slang is not packaged here; shaders are plain HLSL, so a later switch to `slangc` only changes `cmake/LumoraShaders.cmake`. glslang quirks are documented in `shaders/common.hlsli` (`vk::push_constant` only works on `ConstantBuffer<T>`; there is no `NonUniformResourceIndex`) |
| Vulkan C API with thin RAII wrappers | Full control and fast compile times; Vulkan-Hpp RAII adds a lot of compile time |
| One process for Hub and Editor | A requirement. It also lets them share the Vulkan context, log history and recent-project state |
| Lazy Vulkan initialisation | The Hub stays usable on machines without Vulkan 1.4, and failures are reported where the user looks |
| Euler storage for rotation | Lossless round trip through the inspector and files. Gizmos will introduce quaternion editing internally |
