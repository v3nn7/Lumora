# Renderer on NVRHI — port plan and status

Goal: every GPU allocation and command goes through NVRHI (Vulkan now, D3D12 on Windows next).
Our own Vulkan code keeps only what NVRHI does not do: instance/device creation and GPU choice
(`rhi::Instance`, `rhi::Device`, capabilities, error types), the swapchain (`rhi::Swapchain`) and
surface handling. The renderer's public API (`Renderer::render/capture/stats`, `RenderSettings`,
`PresentTarget`, `RenderContext` lookups the app uses) stays the same.

## Mapping

| Before (own Vulkan)                  | After (NVRHI)                                                        |
|--------------------------------------|----------------------------------------------------------------------|
| `rhi::Buffer`, `rhi::Image` (VMA)    | `nvrhi::BufferHandle`, `nvrhi::TextureHandle` (VMA removed)          |
| `UploadQueue` (timeline, staging)    | immediate command list: `writeBuffer`, `writeTexture`, execute       |
| `BindlessTable` (set 1)              | bindless layout {Texture_SRV, Sampler} + `DescriptorTable`, our slot allocator |
| frame descriptor set (set 0)         | binding layout: CB b0, SRV t1..t4 (+ push constants 32 B), offsets 0 |
| `PipelineLayout` + `createGraphicsPipeline` | binding layouts + `GraphicsPipelineDesc` + `FramebufferInfo`  |
| `RenderGraph` barriers (sync2)       | NVRHI state tracking; graph keeps passes, transients, clears, timing |
| dynamic rendering                    | framebuffers per attachment set (cached)                             |
| secondary command buffers            | worker command lists executed in order (`executeCommandLists`)       |
| `GpuProfiler` timestamps             | `TimerQuery` per pass                                                |
| fences / semaphores per frame        | `EventQuery` per frame; swapchain semaphores via `queueWait/SignalSemaphore` |

Shaders keep their `[[vk::binding(b, set)]]` numbers: NVRHI layout 0 uses binding offsets 0
(b0 frame constants, t1-t4 buffers, s5 the four samplers, push constants as b1); the bindless
layout puts textures at binding 0 of set 1. NVRHI presents D3D's clip space (+Y up) on Vulkan by
flipping the viewport, so vertex shaders flip Y (`toTarget`) and the image is the same on both APIs. Entry points keep
their names (`VSMain`, `PSMain`, ...). For D3D12 the same HLSL gets `register()` annotations and is
compiled with DXC to DXIL.

Textures are created with `keepInitialState` so every command list starts and ends with them in a
known state (needed for command lists recorded on several threads).

Mesh buffers: NVRHI makes one allocation per resource; large scenes would hit
`maxMemoryAllocationCount`. Mesh vertex/index data goes into shared heaps (`createHeap`, virtual
buffers bound at offsets).

## Steps

1. [x] NVRHI and Vulkan-Headers 1.4.352 pinned in the build.
2. [x] `RenderContext` creates the NVRHI device over our VkDevice; binding layouts, samplers,
       bindless table, shader library, pipeline creation on NVRHI.
3. [x] Resources and uploads: mesh cache (static buffers in shared 64 MB heaps), texture cache,
       environment cache, HUD atlas.
4. [x] Render graph on NVRHI command lists (transients, framebuffers, clears, timer queries,
       parallel passes).
5. [x] Scene renderer passes: depth prepass, shadows, SSAO, sky, opaque, grid, particles, bloom,
       tonemap, FXAA, overlay, HUD.
6. [x] Renderer frame loop: event queries, swapchain wrap, capture readback, stats.
7. [x] Remove the old resource/upload/bindless/pipeline/graph-barrier code and VMA; tests updated.
8. [ ] D3D12 backend on Windows (device, swapchain via DXGI, DXIL shaders).

Status: steps 1-7 done. Our Vulkan layer keeps the instance, device, swapchain and semaphores;
memory budgets come from VK_EXT_memory_budget directly. Frame command lists are deferred
(not "immediate") so passes can be recorded on several threads. Step 8 needs a Windows machine
to build and test.
