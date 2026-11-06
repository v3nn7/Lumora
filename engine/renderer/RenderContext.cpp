#include "renderer/RenderContext.hpp"

#include "core/Log.hpp"
#include "platform/Paths.hpp"
#include "renderer/GpuTypes.hpp"

#include <algorithm>
#include <format>

namespace lumora::render {

RenderContext::RenderContext(const RenderContextConfig& config) {
    rhi::InstanceConfig ic;
    ic.applicationName = "Lumora";
    ic.enableValidation = config.enableValidation;
    ic.requiredExtensions = config.instanceExtensions;
    instance_ = std::make_unique<rhi::Instance>(ic);

    device_ = std::make_unique<rhi::Device>(*instance_, rhi::DeviceSelection{config.gpuIndex, config.requirePresentation});

    // Validate required formats before building anything on top of them.
    if (!device_->supportsFormat(hdrFormat_, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
                                                 VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT |
                                                 VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
        throw rhi::UnsupportedHardwareError("GPU cannot render to/sample R16G16B16A16_SFLOAT");
    }
    if (!device_->supportsFormat(depthFormat_, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        depthFormat_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
        if (!device_->supportsFormat(depthFormat_, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            throw rhi::UnsupportedHardwareError("GPU supports no 32-bit float depth format");
        }
    }

    uploads_ = std::make_unique<rhi::UploadQueue>(*device_);
    bindless_ = std::make_unique<rhi::BindlessTable>(*device_);
    shaders_ = std::make_unique<rhi::ShaderLibrary>(
        *device_, config.shaderDirectory.empty() ? paths::shaderDir() : config.shaderDirectory);
    pipelineCache_ = std::make_unique<rhi::PipelineCache>(
        *device_, config.pipelineCacheFile.empty() ? paths::userCacheDir() / "pipeline_cache.bin"
                                                   : config.pipelineCacheFile);

    // Set 0: per-frame constants + object buffer.
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateDescriptorSetLayout(device_->handle(), &setInfo, nullptr, &setLayout));
    frameSetLayout_ = rhi::UniqueDescriptorSetLayout(device_->handle(), setLayout);

    const VkDescriptorSetLayout layouts[2] = {setLayout, bindless_->layout()};
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreatePipelineLayout(device_->handle(), &layoutInfo, nullptr, &pipelineLayout));
    pipelineLayout_ = rhi::UniquePipelineLayout(device_->handle(), pipelineLayout);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateSampler(device_->handle(), &samplerInfo, nullptr, &sampler));
    linearClamp_ = rhi::UniqueSampler(device_->handle(), sampler);
    linearClampIndex_ = bindless_->registerSampler(sampler);

    // Hardware PCF: bilinear depth comparison, outside the map counts as lit.
    VkSamplerCreateInfo cmpInfo = samplerInfo;
    cmpInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    cmpInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    cmpInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    cmpInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    cmpInfo.compareEnable = VK_TRUE;
    cmpInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    cmpInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSampler cmpSampler = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateSampler(device_->handle(), &cmpInfo, nullptr, &cmpSampler));
    shadowCompare_ = rhi::UniqueSampler(device_->handle(), cmpSampler);
    shadowCompareIndex_ = bindless_->registerSampler(cmpSampler);

    VkSamplerCreateInfo materialInfo = samplerInfo;
    materialInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    materialInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    materialInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (device_->caps().samplerAnisotropy) {
        materialInfo.anisotropyEnable = VK_TRUE;
        materialInfo.maxAnisotropy = std::min(8.0f, device_->caps().maxAnisotropy);
    }
    VkSampler material = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateSampler(device_->handle(), &materialInfo, nullptr, &material));
    materialSampler_ = rhi::UniqueSampler(device_->handle(), material);
    materialSamplerIndex_ = bindless_->registerSampler(material);

    meshes_ = std::make_unique<MeshCache>(*uploads_);
    textures_ = std::make_unique<TextureCache>(*uploads_, *bindless_);
    environments_ = std::make_unique<EnvironmentCache>(*uploads_, *bindless_);
}

RenderContext::~RenderContext() {
    if (device_) {
        try {
            device_->waitIdle();
        } catch (const std::exception& e) {
            log::error("Renderer", "waitIdle during shutdown failed: {}", e.what());
        }
    }
    // Members are destroyed in reverse declaration order; the device outlives every resource.
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
