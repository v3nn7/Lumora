#include "renderer/RenderContext.hpp"

#include "assets/MeshData.hpp"
#include "core/Log.hpp"
#include "platform/Paths.hpp"
#include "renderer/GpuTypes.hpp"
#include "renderer/HudRenderer.hpp"
#include "renderer/Overlay.hpp"

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <format>

namespace lumora::render {

void initVulkanHppDispatcher(VkInstance instance, VkDevice device);  // VulkanHppDispatch.cpp

namespace {

/// NVRHI's messages go to the engine log; errors also become exceptions in debug builds' tests.
class LogMessages final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* text) override {
        switch (severity) {
            case nvrhi::MessageSeverity::Info: log::debug("NVRHI", "{}", text); break;
            case nvrhi::MessageSeverity::Warning: log::warn("NVRHI", "{}", text); break;
            default: log::error("NVRHI", "{}", text); break;
        }
    }
};

std::vector<nvrhi::VertexAttributeDesc> vertexAttributes(VertexInput input) {
    using A = nvrhi::VertexAttributeDesc;
    using F = nvrhi::Format;
    auto attr = [](const char* name, F format, std::uint32_t buffer, std::uint32_t offset, std::uint32_t stride) {
        return A().setName(name).setFormat(format).setBufferIndex(buffer).setOffset(offset).setElementStride(stride);
    };
    constexpr auto vs = static_cast<std::uint32_t>(sizeof(Vertex));
    constexpr auto ss = static_cast<std::uint32_t>(sizeof(SkinVertex));
    switch (input) {
        case VertexInput::Mesh:
            return {attr("POSITION", F::RGB32_FLOAT, 0, offsetof(Vertex, position), vs),
                    attr("NORMAL", F::RGB32_FLOAT, 0, offsetof(Vertex, normal), vs),
                    attr("TEXCOORD", F::RG32_FLOAT, 0, offsetof(Vertex, uv), vs),
                    attr("TANGENT", F::RGBA32_FLOAT, 0, offsetof(Vertex, tangent), vs)};
        case VertexInput::MeshPosition:
            return {attr("POSITION", F::RGB32_FLOAT, 0, offsetof(Vertex, position), vs)};
        case VertexInput::MeshSkinned:
        case VertexInput::MeshSkinnedPosition:
            // Locations 0-3 for the vertex, 4-5 for the skin, as the skinned shaders declare them
            // (depth-only ones simply ignore the normal, uv and tangent).
            return {attr("POSITION", F::RGB32_FLOAT, 0, offsetof(Vertex, position), vs),
                    attr("NORMAL", F::RGB32_FLOAT, 0, offsetof(Vertex, normal), vs),
                    attr("TEXCOORD", F::RG32_FLOAT, 0, offsetof(Vertex, uv), vs),
                    attr("TANGENT", F::RGBA32_FLOAT, 0, offsetof(Vertex, tangent), vs),
                    attr("BLENDINDICES", F::RGBA16_UINT, 1, offsetof(SkinVertex, joints), ss),
                    attr("BLENDWEIGHT", F::RGBA32_FLOAT, 1, offsetof(SkinVertex, weights), ss)};
        case VertexInput::Overlay:
            return {attr("POSITION", F::RGB32_FLOAT, 0, 0, 16), attr("COLOR", F::RGBA8_UNORM, 0, 12, 16)};
        case VertexInput::Hud:
            return {attr("POSITION", F::RG32_FLOAT, 0, 0, 32), attr("TEXCOORD", F::RG32_FLOAT, 0, 8, 32),
                    attr("COLOR", F::RGBA32_FLOAT, 0, 16, 32)};
        case VertexInput::Particle:
            return {attr("POSITION", F::RGB32_FLOAT, 0, 0, 36), attr("TEXCOORD", F::RG32_FLOAT, 0, 12, 36),
                    attr("COLOR", F::RGBA32_FLOAT, 0, 20, 36)};
        case VertexInput::None:
        default:
            return {};
    }
}

nvrhi::SamplerHandle makeSampler(nvrhi::IDevice* gpu, const nvrhi::SamplerDesc& desc) {
    nvrhi::SamplerHandle s = gpu->createSampler(desc);
    if (!s) throw rhi::VulkanError("could not create a sampler", VK_ERROR_INITIALIZATION_FAILED);
    return s;
}

} // namespace

RenderContext::RenderContext(const RenderContextConfig& config) {
    rhi::InstanceConfig ic;
    ic.applicationName = "Lumora";
    ic.enableValidation = config.enableValidation;
    ic.requiredExtensions = config.instanceExtensions;
    instance_ = std::make_unique<rhi::Instance>(ic);
    device_ = std::make_unique<rhi::Device>(*instance_, rhi::DeviceSelection{config.gpuIndex, config.requirePresentation});

    if (!device_->supportsFormat(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
                                                                    VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT |
                                                                    VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
        throw rhi::UnsupportedHardwareError("GPU cannot render to/sample R16G16B16A16_SFLOAT");
    }
    if (!device_->supportsFormat(VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        depthFormat_ = nvrhi::Format::D32S8;
        if (!device_->supportsFormat(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            throw rhi::UnsupportedHardwareError("GPU supports no 32-bit float depth format");
        }
    }

    // NVRHI on the device Lumora chose and created.
    static LogMessages messages;  // stateless, shared by every context
    std::vector<const char*> instanceExtensions;
    for (const auto& e : instance_->enabledExtensions()) instanceExtensions.push_back(e.c_str());
    std::vector<const char*> deviceExtensions;
    for (const auto& e : device_->enabledExtensions()) deviceExtensions.push_back(e.c_str());
    nvrhi::vulkan::DeviceDesc desc;
    desc.errorCB = &messages;
    desc.instance = instance_->handle();
    desc.physicalDevice = device_->physical();
    desc.device = device_->handle();
    desc.graphicsQueue = device_->graphicsQueue();
    desc.graphicsQueueIndex = static_cast<int>(device_->graphicsFamily());
    desc.instanceExtensions = instanceExtensions.data();
    desc.numInstanceExtensions = instanceExtensions.size();
    desc.deviceExtensions = deviceExtensions.data();
    desc.numDeviceExtensions = deviceExtensions.size();
    initVulkanHppDispatcher(instance_->handle(), device_->handle());
    vulkanDevice_ = nvrhi::vulkan::createDevice(desc);
    if (!vulkanDevice_) throw rhi::VulkanError("NVRHI could not use the Vulkan device", VK_ERROR_INITIALIZATION_FAILED);
    vulkanGpu_ = static_cast<nvrhi::vulkan::IDevice*>(vulkanDevice_.Get());
    gpu_ = vulkanDevice_;
    if (config.enableValidation) {
        validated_ = nvrhi::validation::createValidationLayer(vulkanDevice_);
        gpu_ = validated_;
    }

    bindless_ = std::make_unique<BindlessTable>(gpu_);
    shaders_ = std::make_unique<ShaderLibrary>(gpu_, config.shaderDirectory.empty() ? paths::shaderDir()
                                                                                    : config.shaderDirectory);
    // Set 0 keeps the shaders' binding numbers: no per-type offsets.
    nvrhi::BindingLayoutDesc frame;
    frame.visibility = nvrhi::ShaderType::All;
    frame.registerSpace = 0;
    frame.registerSpaceIsDescriptorSet = true;
    frame.bindingOffsets.setShaderResourceOffset(0).setSamplerOffset(0).setConstantBufferOffset(0).setUnorderedAccessViewOffset(0);
    frame.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0),        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
                      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),  nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
                      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
                      nvrhi::BindingLayoutItem::Sampler(5).setSize(4),
                      nvrhi::BindingLayoutItem::PushConstants(1, sizeof(PushConstants))};  // b1: b0 is the frame CB
    frameLayout_ = gpu_->createBindingLayout(frame);
    if (!frameLayout_) throw rhi::VulkanError("could not create the frame binding layout", VK_ERROR_INITIALIZATION_FAILED);
    uploadList_ = gpu_->createCommandList();

    auto linear = nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    linearClampIndex_ = 0;
    samplers_[0] = makeSampler(gpu_, linear);
    // Hardware PCF: bilinear depth comparison, outside the map counts as lit.
    auto compare = nvrhi::SamplerDesc()
                       .setAllFilters(true)
                       .setMipFilter(false)
                       .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
                       .setBorderColor(nvrhi::Color(1.0f))
                       .setReductionType(nvrhi::SamplerReductionType::Comparison);
    shadowCompareIndex_ = 1;
    samplers_[1] = makeSampler(gpu_, compare);
    auto material = nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    if (device_->caps().samplerAnisotropy) material.setMaxAnisotropy(std::min(8.0f, device_->caps().maxAnisotropy));
    materialSamplerIndex_ = 2;
    samplers_[2] = makeSampler(gpu_, material);
    samplers_[3] = samplers_[0];  // spare

    meshes_ = std::make_unique<MeshCache>(*this);
    textures_ = std::make_unique<TextureCache>(*this);
    environments_ = std::make_unique<EnvironmentCache>(*this);
}

RenderContext::~RenderContext() {
    if (gpu_ != nullptr) gpu_->waitForIdle();
    environments_.reset();
    textures_.reset();
    meshes_.reset();
    uploadList_ = nullptr;
    bufferHeaps_.clear();
    if (gpu_ != nullptr) gpu_->runGarbageCollection();
    // NVRHI objects go before the Vulkan device they live on (members below the device).
    samplers_ = {};
    frameLayout_ = nullptr;
    shaders_.reset();
    bindless_.reset();
    validated_ = nullptr;
    gpu_ = nullptr;
    vulkanGpu_ = nullptr;
    vulkanDevice_ = nullptr;
}

nvrhi::GraphicsPipelineHandle RenderContext::createPipeline(const PipelineDesc& d) {
    nvrhi::GraphicsPipelineDesc desc;
    desc.VS = shaders_->get(d.vertexShader, d.vertexEntry);
    if (!d.pixelShader.empty()) desc.PS = shaders_->get(d.pixelShader, d.pixelEntry);
    const auto attributes = vertexAttributes(d.input);
    if (!attributes.empty()) {
        desc.inputLayout = gpu_->createInputLayout(attributes.data(), static_cast<std::uint32_t>(attributes.size()), desc.VS);
    }
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.bindingLayouts = {frameLayout_, bindless_->layout()};
    auto& raster = desc.renderState.rasterState;
    raster.setCullMode(d.cull).setFrontCounterClockwise(true).setDepthClipEnable(true);
    raster.depthBias = d.depthBias;
    raster.slopeScaledDepthBias = d.slopeScaledDepthBias;
    auto& depth = desc.renderState.depthStencilState;
    depth.setDepthTestEnable(d.depthTest).setDepthWriteEnable(d.depthWrite).setDepthFunc(d.depthFunc).setStencilEnable(false);
    for (std::size_t i = 0; i < d.colorFormats.size(); ++i) {
        auto& target = desc.renderState.blendState.targets[i];
        switch (d.blend) {
            case BlendMode::Opaque: target.setBlendEnable(false); break;
            case BlendMode::Alpha:
                target.setBlendEnable(true)
                    .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                    .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
                break;
            case BlendMode::Additive:
                target.setBlendEnable(true)
                    .setSrcBlend(nvrhi::BlendFactor::One)
                    .setDestBlend(nvrhi::BlendFactor::One)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::One);
                break;
        }
    }
    nvrhi::FramebufferInfo info;
    for (const nvrhi::Format f : d.colorFormats) info.colorFormats.push_back(f);
    info.depthFormat = d.depthFormat;
    nvrhi::GraphicsPipelineHandle pipeline = gpu_->createGraphicsPipeline(desc, info);
    if (!pipeline) throw rhi::VulkanError("could not create pipeline " + d.name, VK_ERROR_INITIALIZATION_FAILED);
    return pipeline;
}

nvrhi::BufferHandle RenderContext::createBuffer(std::span<const std::byte> bytes, bool vertex, bool index,
                                                const char* name) {
    nvrhi::BufferDesc desc;
    desc.byteSize = std::max<std::uint64_t>(bytes.size(), 4);
    desc.debugName = name;
    desc.isVertexBuffer = vertex;
    desc.isIndexBuffer = index;
    desc.structStride = 0;
    desc.canHaveRawViews = !vertex && !index;
    desc.initialState = vertex ? nvrhi::ResourceStates::VertexBuffer
                               : index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.isVirtual = true;  // memory from a shared heap
    nvrhi::BufferHandle buffer = gpu_->createBuffer(desc);
    if (buffer && !placeInHeap(buffer)) buffer = nullptr;
    if (!buffer) {  // e.g. larger than a heap: a buffer of its own
        desc.isVirtual = false;
        buffer = gpu_->createBuffer(desc);
    }
    if (!buffer) throw rhi::VulkanError(std::string("could not create buffer ") + name, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    if (!bytes.empty()) upload([&](nvrhi::ICommandList* cmd) { cmd->writeBuffer(buffer, bytes.data(), bytes.size()); });
    return buffer;
}

bool RenderContext::placeInHeap(nvrhi::IBuffer* buffer) {
    constexpr std::uint64_t kHeapSize = 64ull << 20;
    const nvrhi::MemoryRequirements req = gpu_->getBufferMemoryRequirements(buffer);
    if (req.size == 0 || req.size > kHeapSize) return false;
    const std::uint64_t alignment = std::max<std::uint64_t>(req.alignment, 1);
    for (BufferHeap& h : bufferHeaps_) {
        const std::uint64_t offset = (h.used + alignment - 1) / alignment * alignment;
        if (offset + req.size <= kHeapSize && gpu_->bindBufferMemory(buffer, h.heap, offset)) {
            h.used = offset + req.size;
            return true;
        }
    }
    nvrhi::HeapDesc desc;
    desc.capacity = kHeapSize;
    desc.type = nvrhi::HeapType::DeviceLocal;
    desc.debugName = "MeshHeap";
    BufferHeap heap{gpu_->createHeap(desc), 0};
    if (!heap.heap || !gpu_->bindBufferMemory(buffer, heap.heap, 0)) return false;
    heap.used = req.size;
    bufferHeaps_.push_back(std::move(heap));
    return true;
}

nvrhi::TextureHandle RenderContext::createTexture(std::span<const std::span<const std::byte>> levels, std::uint32_t width,
                                                  std::uint32_t height, nvrhi::Format format, const char* name) {
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = static_cast<std::uint32_t>(levels.size());
    desc.format = format;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::TextureHandle texture = gpu_->createTexture(desc);
    if (!texture) throw rhi::VulkanError(std::string("could not create texture ") + name, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(format);
    upload([&](nvrhi::ICommandList* cmd) {
        for (std::uint32_t mip = 0; mip < levels.size(); ++mip) {
            const std::uint32_t w = std::max(width >> mip, 1u);
            cmd->writeTexture(texture, 0, mip, levels[mip].data(), static_cast<std::size_t>(w) * info.bytesPerBlock);
        }
    });
    return texture;
}

std::string RenderContext::describeDevices(bool requirePresentation) {
    rhi::InstanceConfig ic;
    ic.applicationName = "Lumora (device query)";
    rhi::Instance instance(ic);
    std::string out = std::format("Vulkan loader: {}\n", rhi::versionString(instance.loaderVersion()));
    for (const auto& c : rhi::enumerateDevices(instance.handle(), requirePresentation)) {
        const auto& caps = c.caps;
        out += std::format("[{}] {}\n    type: {}  driver: {} {}\n    Vulkan: {}  VRAM: {} MiB\n", c.index, caps.name,
                           caps.typeName(), caps.driverName, caps.driverInfo, rhi::versionString(caps.apiVersion),
                           caps.deviceLocalBytes >> 20);
        out += std::format("    timestamps: {}  ray tracing: {}  ray query: {}  mesh shaders: {}\n",
                           caps.timestamps ? "yes" : "no",
                           caps.rayTracingPipeline && caps.accelerationStructure ? "yes" : "no",
                           caps.rayQuery ? "yes" : "no", caps.meshShader ? "yes" : "no");
        out += std::format("    status: {}\n", c.suitable ? "SUITABLE" : "unsuitable - " + c.rejectReason);
    }
    return out;
}

} // namespace lumora::render
