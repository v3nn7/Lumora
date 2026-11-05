#include "renderer/vulkan/Pipeline.hpp"

#include "assets/MeshData.hpp"
#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "renderer/vulkan/Device.hpp"

#include <cstddef>
#include <cstring>
#include <format>

namespace lumora::rhi {

ShaderLibrary::ShaderLibrary(const Device& device, std::filesystem::path directory)
    : device_(device), directory_(std::move(directory)) {}

VkShaderModule ShaderLibrary::get(const std::string& name) {
    if (auto it = modules_.find(name); it != modules_.end()) {
        return it->second.get();
    }
    const auto path = directory_ / (name + ".spv");
    auto bytes = fs::readBinaryFile(path);
    if (!bytes) {
        throw VulkanError(std::format("shader '{}' not found ({}). Was the build's shader step run?", name,
                                      bytes.error().message),
                          VK_ERROR_INITIALIZATION_FAILED);
    }
    if (bytes->size() < 20 || bytes->size() % 4 != 0) {
        throw VulkanError(std::format("shader '{}' is not valid SPIR-V", path.string()), VK_ERROR_INITIALIZATION_FAILED);
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes->data(), 4);
    if (magic != 0x07230203u) {
        throw VulkanError(std::format("shader '{}' has a bad SPIR-V magic number", path.string()),
                          VK_ERROR_INITIALIZATION_FAILED);
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes->size();
    info.pCode = reinterpret_cast<const std::uint32_t*>(bytes->data());
    VkShaderModule module = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateShaderModule(device_.handle(), &info, nullptr, &module));
    device_.setName(VK_OBJECT_TYPE_SHADER_MODULE, module, name.c_str());
    log::debug("Renderer", "loaded shader {}", path.string());
    return modules_.emplace(name, UniqueShaderModule(device_.handle(), module)).first->second.get();
}

PipelineCache::PipelineCache(const Device& device, std::filesystem::path file) : device_(device), file_(std::move(file)) {
    std::vector<std::byte> initial;
    if (auto data = fs::readBinaryFile(file_); data && data->size() >= sizeof(VkPipelineCacheHeaderVersionOne)) {
        VkPipelineCacheHeaderVersionOne header{};
        std::memcpy(&header, data->data(), sizeof(header));
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device_.physical(), &props);
        if (header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendorID == props.vendorID &&
            header.deviceID == props.deviceID &&
            std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0) {
            initial = std::move(*data);
            log::info("Renderer", "pipeline cache loaded ({} KiB)", initial.size() / 1024);
        } else {
            log::info("Renderer", "pipeline cache is from another driver/device - starting fresh");
        }
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = initial.size();
    info.pInitialData = initial.empty() ? nullptr : initial.data();
    LUMORA_VK_CHECK(vkCreatePipelineCache(device_.handle(), &info, nullptr, &cache_));
}

PipelineCache::~PipelineCache() {
    save();
    vkDestroyPipelineCache(device_.handle(), cache_, nullptr);
}

void PipelineCache::save() const {
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device_.handle(), cache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<std::byte> data(size);
    if (vkGetPipelineCacheData(device_.handle(), cache_, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);
    if (auto r = fs::writeFileAtomic(file_, data); !r) {
        log::warn("Renderer", "could not save pipeline cache: {}", r.error().message);
    }
}

UniquePipeline createGraphicsPipeline(const Device& device, const PipelineCache& cache, const GraphicsPipelineDesc& d) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = d.vertexShader;
    stages[0].pName = d.vertexEntry;
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = d.fragmentShader;
    stages[1].pName = d.fragmentEntry;

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, normal))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, uv))},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d.meshVertexInput) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = d.positionOnly ? 1u : 3u;
        vertexInput.pVertexAttributeDescriptions = attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = d.cullMode;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = d.depthBias ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = d.depthBiasConstant;
    raster.depthBiasSlopeFactor = d.depthBiasSlope;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = d.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = d.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = d.depthCompare;

    std::vector<VkPipelineColorBlendAttachmentState> blends(d.colorFormats.size());
    for (auto& b : blends) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
        if (d.blend == BlendMode::Alpha) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.colorBlendOp = VK_BLEND_OP_ADD;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (d.blend == BlendMode::Additive) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.colorBlendOp = VK_BLEND_OP_ADD;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<std::uint32_t>(blends.size());
    blend.pAttachments = blends.data();

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(d.colorFormats.size());
    rendering.pColorAttachmentFormats = d.colorFormats.data();
    rendering.depthAttachmentFormat = d.depthFormat;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = d.fragmentShader != VK_NULL_HANDLE ? 2u : 1u;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = d.layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    LUMORA_VK_CHECK(vkCreateGraphicsPipelines(device.handle(), cache.handle(), 1, &info, nullptr, &pipeline));
    if (d.debugName != nullptr) device.setName(VK_OBJECT_TYPE_PIPELINE, pipeline, d.debugName);
    return {device.handle(), pipeline};
}

} // namespace lumora::rhi
