#include "renderer/RenderGraph.hpp"

#include "core/Log.hpp"
#include "renderer/GpuProfiler.hpp"
#include "renderer/TransientImagePool.hpp"
#include "renderer/vulkan/Device.hpp"
#include "renderer/vulkan/Resources.hpp"

#include <algorithm>

namespace lumora::render {

ImageState stateForUsage(ImageUsage usage) noexcept {
    switch (usage) {
        case ImageUsage::ColorAttachment:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case ImageUsage::DepthAttachment:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case ImageUsage::DepthReadOnly:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
        case ImageUsage::FragmentSampled:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case ImageUsage::TransferSrc:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
        case ImageUsage::TransferDst:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case ImageUsage::Present:
            return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    }
    return {};
}

VkImageUsageFlags imageUsageFlags(ImageUsage usage) noexcept {
    switch (usage) {
        case ImageUsage::ColorAttachment: return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        case ImageUsage::DepthAttachment:
        case ImageUsage::DepthReadOnly: return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        case ImageUsage::FragmentSampled: return VK_IMAGE_USAGE_SAMPLED_BIT;
        case ImageUsage::TransferSrc: return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        case ImageUsage::TransferDst: return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        case ImageUsage::Present: return 0;
    }
    return 0;
}

bool hasWriteAccess(VkAccessFlags2 access) noexcept {
    constexpr VkAccessFlags2 kWrites =
        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT;
    return (access & kWrites) != 0;
}

std::optional<VkImageMemoryBarrier2> makeImageBarrier(const ImageState& from, const ImageState& to, VkImage image,
                                                      VkImageAspectFlags aspect, bool discard) {
    const bool layoutChange = discard || from.layout != to.layout;
    if (!layoutChange && !hasWriteAccess(from.access) && !hasWriteAccess(to.access)) {
        return std::nullopt;  // read after read
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = from.stages;
    // Only writes need to be made available; read->write hazards need just the execution dependency.
    b.srcAccessMask = hasWriteAccess(from.access) ? from.access : VK_ACCESS_2_NONE;
    b.dstStageMask = to.stages;
    b.dstAccessMask = to.access;
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : from.layout;
    b.newLayout = to.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return b;
}

// ---------------------------------------------------------------------------------------------

void PassBuilder::writeColor(RGImage image, LoadOp load, glm::vec4 clear) {
    RenderGraph::Access a{image, ImageUsage::ColorAttachment, load, {}};
    a.clear.color = {{clear.r, clear.g, clear.b, clear.a}};
    graph_.addAccess(pass_, a);
}

void PassBuilder::writeDepth(RGImage image, LoadOp load, float clearDepth) {
    RenderGraph::Access a{image, ImageUsage::DepthAttachment, load, {}};
    a.clear.depthStencil = {clearDepth, 0};
    graph_.addAccess(pass_, a);
}

void PassBuilder::readDepth(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::DepthReadOnly, LoadOp::Load, {}}); }
void PassBuilder::sample(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::FragmentSampled, LoadOp::Load, {}}); }
void PassBuilder::copyFrom(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::TransferSrc, LoadOp::Load, {}}); }
void PassBuilder::copyTo(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::TransferDst, LoadOp::DontCare, {}}); }

VkImage PassContext::image(RGImage h) const { return graph_.resources_.at(h.id).physical.image; }
VkImageView PassContext::view(RGImage h) const { return graph_.resources_.at(h.id).physical.view; }
VkExtent2D PassContext::extent(RGImage h) const { return graph_.resources_.at(h.id).physical.extent; }
std::uint32_t PassContext::bindlessIndex(RGImage h) const { return graph_.resources_.at(h.id).physical.bindlessIndex; }

// ---------------------------------------------------------------------------------------------

RGImage RenderGraph::importImage(std::string name, const ImportedImage& image) {
    resources_.push_back({std::move(name), true, image, {image.format, image.extent}});
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

RGImage RenderGraph::createImage(std::string name, const TransientImageDesc& desc) {
    Resource r;
    r.name = std::move(name);
    r.desc = desc;
    r.physical.format = desc.format;
    r.physical.extent = desc.extent;
    resources_.push_back(std::move(r));
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

void RenderGraph::addRasterPass(std::string name, const SetupFn& setup, ExecuteFn execute) {
    passes_.push_back({std::move(name), true, {}, std::move(execute)});
    PassBuilder builder(*this, static_cast<std::uint32_t>(passes_.size() - 1));
    setup(builder);
}

void RenderGraph::addPass(std::string name, const SetupFn& setup, ExecuteFn execute) {
    passes_.push_back({std::move(name), false, {}, std::move(execute)});
    PassBuilder builder(*this, static_cast<std::uint32_t>(passes_.size() - 1));
    setup(builder);
}

void RenderGraph::addAccess(std::uint32_t pass, const Access& access) {
    if (!access.image.valid() || access.image.id >= resources_.size()) {
        throw rhi::VulkanError("render graph: pass '" + passes_[pass].name + "' uses an invalid image handle",
                               VK_ERROR_UNKNOWN);
    }
    passes_[pass].accesses.push_back(access);
}

VkImageUsageFlags RenderGraph::requiredUsage(RGImage image) const {
    VkImageUsageFlags flags = 0;
    for (const Pass& p : passes_) {
        for (const Access& a : p.accesses) {
            if (a.image == image) flags |= imageUsageFlags(a.usage);
        }
    }
    return flags;
}

std::vector<RenderGraph::PlannedBarrier> RenderGraph::planBarriers() const {
    std::vector<PlannedBarrier> planned;
    std::vector<ImageState> current(resources_.size());
    std::vector<bool> touched(resources_.size(), false);
    for (std::size_t i = 0; i < resources_.size(); ++i) {
        current[i] = resources_[i].imported ? resources_[i].physical.initialState : ImageState{};
    }
    for (std::uint32_t p = 0; p < passes_.size(); ++p) {
        for (const Access& a : passes_[p].accesses) {
            const std::uint32_t id = a.image.id;
            const ImageState next = stateForUsage(a.usage);
            // Transients start each frame with undefined contents; a clear/don't-care write
            // to anything also makes the previous contents irrelevant.
            const bool firstTransientUse = !resources_[id].imported && !touched[id];
            const bool overwrite = (a.usage == ImageUsage::ColorAttachment || a.usage == ImageUsage::DepthAttachment ||
                                    a.usage == ImageUsage::TransferDst) &&
                                   a.load != LoadOp::Load;
            const bool discard = firstTransientUse || overwrite;
            if (firstTransientUse && a.load == LoadOp::Load) {
                log::warn("RenderGraph", "pass '{}' loads transient image '{}' before anything wrote it",
                          passes_[p].name, resources_[id].name);
            }
            const ImageState& from = current[id];
            const bool layoutChange = discard || from.layout != next.layout;
            if (layoutChange || hasWriteAccess(from.access) || hasWriteAccess(next.access)) {
                planned.push_back({p, a.image, from, next, discard});
            }
            current[id] = next;
            touched[id] = true;
        }
    }
    for (std::uint32_t id = 0; id < resources_.size(); ++id) {
        const Resource& r = resources_[id];
        if (r.imported && r.physical.finalUsage) {
            const ImageState next = stateForUsage(*r.physical.finalUsage);
            if (next != current[id]) {
                planned.push_back({UINT32_MAX, {id}, current[id], next, false});
            }
        }
    }
    return planned;
}

void RenderGraph::execute(VkCommandBuffer cmd, const rhi::Device& device, TransientImagePool& pool,
                          GpuProfiler* profiler) {
    // 1. Realise transient images.
    for (std::uint32_t id = 0; id < resources_.size(); ++id) {
        Resource& r = resources_[id];
        if (r.imported) continue;
        const VkImageUsageFlags usage = requiredUsage({id});
        if (usage == 0) continue;  // declared but never used
        const PooledImage& img = pool.acquire(r.desc.format, r.desc.extent, usage, r.name.c_str());
        r.physical.image = img.image.handle();
        r.physical.view = img.image.view();
        r.physical.bindlessIndex = img.bindlessIndex;
    }

    // 2. Group planned barriers per pass.
    const auto planned = planBarriers();
    auto emitBarriers = [&](std::uint32_t pass) {
        std::vector<VkImageMemoryBarrier2> barriers;
        for (const PlannedBarrier& b : planned) {
            if (b.pass != pass) continue;
            const Resource& r = resources_[b.image.id];
            const VkImageAspectFlags aspect =
                rhi::isDepthFormat(r.physical.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            if (auto barrier = makeImageBarrier(b.from, b.to, r.physical.image, aspect, b.discard)) {
                barriers.push_back(*barrier);
            }
        }
        if (barriers.empty()) return;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    static constexpr float kLabelColor[4] = {0.35f, 0.65f, 1.0f, 1.0f};
    PassContext ctx(*this);
    ctx.cmd = cmd;
    for (std::uint32_t p = 0; p < passes_.size(); ++p) {
        Pass& pass = passes_[p];
        emitBarriers(p);
        device.beginLabel(cmd, pass.name.c_str(), kLabelColor);
        const std::uint32_t scope = profiler != nullptr ? profiler->beginScope(cmd, pass.name) : UINT32_MAX;

        if (pass.raster) {
            std::vector<VkRenderingAttachmentInfo> colors;
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            bool hasDepth = false;
            VkExtent2D extent{0, 0};
            for (const Access& a : pass.accesses) {
                const Resource& r = resources_[a.image.id];
                const bool isColor = a.usage == ImageUsage::ColorAttachment;
                const bool isDepth = a.usage == ImageUsage::DepthAttachment || a.usage == ImageUsage::DepthReadOnly;
                if (!isColor && !isDepth) continue;
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = r.physical.view;
                info.imageLayout = stateForUsage(a.usage).layout;
                info.loadOp = a.load == LoadOp::Clear      ? VK_ATTACHMENT_LOAD_OP_CLEAR
                              : a.load == LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                           : VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = a.usage == ImageUsage::DepthReadOnly ? VK_ATTACHMENT_STORE_OP_NONE
                                                                    : VK_ATTACHMENT_STORE_OP_STORE;
                info.clearValue = a.clear;
                extent = r.physical.extent;
                if (isColor) {
                    colors.push_back(info);
                } else {
                    depth = info;
                    hasDepth = true;
                }
            }
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = hasDepth ? &depth : nullptr;
            vkCmdBeginRendering(cmd, &rendering);
            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                      0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            pass.execute(ctx);
            vkCmdEndRendering(cmd);
        } else {
            pass.execute(ctx);
        }

        if (profiler != nullptr) profiler->endScope(cmd, scope);
        device.endLabel(cmd);
    }
    emitBarriers(UINT32_MAX);
}

} // namespace lumora::render
