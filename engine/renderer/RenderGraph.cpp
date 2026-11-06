#include "renderer/RenderGraph.hpp"

#include "core/JobSystem.hpp"
#include "renderer/GpuProfiler.hpp"
#include "renderer/TransientImagePool.hpp"
#include "renderer/vulkan/VulkanCommon.hpp"

#include <algorithm>
#include <utility>

namespace lumora::render {

// ---- FramebufferCache --------------------------------------------------------------------------

nvrhi::IFramebuffer* FramebufferCache::get(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc, std::uint64_t frame) {
    std::vector<const void*> key;
    for (const auto& a : desc.colorAttachments) key.push_back(a.texture);
    key.push_back(desc.depthAttachment.texture);
    key.push_back(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(desc.depthAttachment.isReadOnly ? 1 : 0)));
    Entry& entry = entries_[key];
    if (!entry.framebuffer) {
        entry.framebuffer = device->createFramebuffer(desc);
        if (!entry.framebuffer) throw rhi::VulkanError("could not create a framebuffer", VK_ERROR_INITIALIZATION_FAILED);
    }
    entry.lastUsed = frame;
    return entry.framebuffer;
}

void FramebufferCache::prune(std::uint64_t frame) {
    std::erase_if(entries_, [frame](const auto& e) { return e.second.lastUsed + 8 < frame; });
}

// ---- PassBuilder ---------------------------------------------------------------------------------

void PassBuilder::writeColor(RGImage image, LoadOp load, glm::vec4 clear) {
    graph_.addAccess(pass_, {image, ImageUsage::ColorAttachment, load, clear});
}

void PassBuilder::writeDepth(RGImage image, LoadOp load, float clearDepth) {
    graph_.addAccess(pass_, {image, ImageUsage::DepthAttachment, load, glm::vec4(clearDepth)});
}

void PassBuilder::readDepth(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::DepthReadOnly}); }
void PassBuilder::sample(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::Sampled}); }
void PassBuilder::copyFrom(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::CopySource}); }
void PassBuilder::copyTo(RGImage image) { graph_.addAccess(pass_, {image, ImageUsage::CopyDestination, LoadOp::DontCare}); }
void PassBuilder::recordInParallel(bool parallel) { graph_.passes_.at(pass_).parallel = parallel; }

// ---- PassContext ---------------------------------------------------------------------------------

nvrhi::ITexture* PassContext::texture(RGImage h) const { return graph_.resource(h).physical.texture; }
std::uint32_t PassContext::bindlessIndex(RGImage h) const { return graph_.resource(h).physical.bindlessIndex; }

nvrhi::ViewportState PassContext::viewport() const {
    const nvrhi::FramebufferInfoEx& info = framebuffer->getFramebufferInfo();
    return nvrhi::ViewportState().addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
}

void PassContext::prepareSampled(nvrhi::ICommandList* list) const {
    // Bindless reads are invisible to NVRHI's tracking: declare them.
    bool any = false;
    for (const RenderGraph::Access& a : graph_.passes_[pass_].accesses) {
        if (a.usage != ImageUsage::Sampled) continue;
        list->setTextureState(graph_.resource(a.image).physical.texture, nvrhi::AllSubresources,
                              nvrhi::ResourceStates::ShaderResource);
        any = true;
    }
    if (any) list->commitBarriers();
}

void PassContext::recordParallel(std::size_t count, std::size_t grain, const ChunkFn& fn) {
    lastChunks_ = 0;
    if (count == 0) return;
    const RenderGraph::Pass& pass = graph_.passes_[pass_];
    grain = std::max<std::size_t>(grain, 1);
    const std::size_t chunks = std::min<std::size_t>((count + grain - 1) / grain, runtime_.maxThreads);
    if (!pass.parallel || !runtime_.allowParallel || !runtime_.newList || chunks < 2) {
        fn(cmd, 0, count);
        return;
    }
    // The pass so far ends this list; the chunks follow in their own lists, then a new list
    // continues with whatever the pass and the next passes record.
    cmd->close();
    std::vector<nvrhi::CommandListHandle> lists(chunks);
    for (auto& list : lists) {
        list = runtime_.newList();
        prepareSampled(list);
    }
    const std::size_t per = (count + chunks - 1) / chunks;
    JobSystem::global().parallelFor(chunks, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t c = begin; c < end; ++c) {
            const std::size_t first = c * per;
            const std::size_t last = std::min(count, first + per);
            if (first < last) fn(lists[c], first, last);
        }
    });
    for (auto& list : lists) {
        list->close();
        graph_.lists_.push_back(list);
    }
    nvrhi::CommandListHandle next = runtime_.newList();
    graph_.lists_.push_back(next);
    cmd = next;
    lastChunks_ = static_cast<std::uint32_t>(chunks);
    graph_.parallelChunks_ += lastChunks_;
}

// ---- RenderGraph ---------------------------------------------------------------------------------

RGImage RenderGraph::importImage(std::string name, const ImportedImage& image) {
    Resource r;
    r.name = std::move(name);
    r.imported = true;
    r.physical = image;
    if (image.texture) r.desc = {image.texture->getDesc().format, image.texture->getDesc().width, image.texture->getDesc().height};
    resources_.push_back(std::move(r));
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

RGImage RenderGraph::createImage(std::string name, const TransientImageDesc& desc) {
    Resource r;
    r.name = std::move(name);
    r.desc = desc;
    resources_.push_back(std::move(r));
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

void RenderGraph::addRasterPass(std::string name, const SetupFn& setup, ExecuteFn execute) {
    passes_.push_back({std::move(name), true, false, {}, std::move(execute)});
    PassBuilder builder(*this, static_cast<std::uint32_t>(passes_.size() - 1));
    setup(builder);
}

void RenderGraph::addPass(std::string name, const SetupFn& setup, ExecuteFn execute) {
    passes_.push_back({std::move(name), false, false, {}, std::move(execute)});
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

const RenderGraph::Resource& RenderGraph::resource(RGImage h) const { return resources_.at(h.id); }

std::vector<nvrhi::CommandListHandle> RenderGraph::execute(nvrhi::CommandListHandle first, GraphRuntime& runtime) {
    parallelChunks_ = 0;
    lists_.clear();
    lists_.push_back(first);
    // Transients come from the pool (recycled per frame slot).
    for (Resource& r : resources_) {
        if (r.imported) continue;
        const PooledImage& image = runtime.pool->acquire(r.desc.format, r.desc.width, r.desc.height, r.name.c_str());
        r.physical.texture = image.texture;
        r.physical.bindlessIndex = image.bindlessIndex;
    }
    for (std::uint32_t p = 0; p < passes_.size(); ++p) {
        Pass& pass = passes_[p];
        nvrhi::ICommandList* cmd = lists_.back();
        const std::uint32_t scope = runtime.profiler != nullptr ? runtime.profiler->beginScope(cmd, pass.name) : UINT32_MAX;
        PassContext ctx(*this, runtime, p);
        if (pass.raster) {
            nvrhi::FramebufferDesc fb;
            for (const Access& a : pass.accesses) {
                nvrhi::ITexture* texture = resource(a.image).physical.texture;
                switch (a.usage) {
                    case ImageUsage::ColorAttachment:
                        fb.addColorAttachment(texture);
                        if (a.load == LoadOp::Clear) {
                            cmd->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                   nvrhi::Color(a.clear.r, a.clear.g, a.clear.b, a.clear.a));
                        }
                        break;
                    case ImageUsage::DepthAttachment:
                        fb.setDepthAttachment(texture);
                        if (a.load == LoadOp::Clear) {
                            cmd->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, a.clear.r, false, 0);
                        }
                        break;
                    case ImageUsage::DepthReadOnly:
                        fb.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(texture).setReadOnly(true));
                        break;
                    default:
                        break;
                }
            }
            // Framebuffers over imported images (the swapchain) are made per frame: those images
            // come and go with their window; the rest are cached.
            const bool imported = std::ranges::any_of(pass.accesses, [&](const Access& a) {
                return resource(a.image).imported && a.usage != ImageUsage::Sampled;
            });
            if (imported) {
                nvrhi::FramebufferHandle handle = runtime.device->createFramebuffer(fb);
                if (!handle) throw rhi::VulkanError("could not create a framebuffer", VK_ERROR_INITIALIZATION_FAILED);
                ownedFramebuffers_.push_back(handle);
                ctx.framebuffer = handle;
            } else {
                ctx.framebuffer = runtime.framebuffers->get(runtime.device, fb, runtime.frame);
            }
        }
        ctx.cmd = cmd;
        ctx.prepareSampled(cmd);
        pass.execute(ctx);
        if (runtime.profiler != nullptr) runtime.profiler->endScope(lists_.back(), scope);
    }
    // Lists replaced by recordParallel were closed there; the caller closes the last one.
    return std::exchange(lists_, {});
}

} // namespace lumora::render
