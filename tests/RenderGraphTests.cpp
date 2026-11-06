// Render graph bookkeeping (no GPU). Execution on the GPU is covered by the renderer tests.
#include "renderer/RenderGraph.hpp"
#include "renderer/vulkan/VulkanCommon.hpp"

#include <gtest/gtest.h>

using namespace lumora::render;

TEST(RenderGraph, RecordsPassesAndImagesInOrder) {
    RenderGraph graph;
    const RGImage hdr = graph.createImage("HDR", {nvrhi::Format::RGBA16_FLOAT, 64, 32});
    const RGImage depth = graph.createImage("Depth", {nvrhi::Format::D32, 64, 32});
    graph.addRasterPass(
        "Prepass", [&](PassBuilder& b) { b.writeDepth(depth, LoadOp::Clear, 0.0f); }, [](PassContext&) {});
    graph.addRasterPass(
        "Opaque",
        [&](PassBuilder& b) {
            b.writeColor(hdr, LoadOp::Clear);
            b.readDepth(depth);
            b.recordInParallel();
        },
        [](PassContext&) {});
    graph.addPass("Copy", [&](PassBuilder& b) { b.copyFrom(hdr); }, [](PassContext&) {});
    ASSERT_EQ(graph.passCount(), 3u);
    EXPECT_EQ(graph.passName(0), "Prepass");
    EXPECT_EQ(graph.passName(2), "Copy");
    EXPECT_EQ(graph.imageCount(), 2u);
}

TEST(RenderGraph, RejectsInvalidHandles) {
    RenderGraph graph;
    EXPECT_THROW(graph.addRasterPass(
                     "Bad", [](PassBuilder& b) { b.writeColor(RGImage{}, LoadOp::Clear); }, [](PassContext&) {}),
                 lumora::rhi::VulkanError);
    EXPECT_THROW(graph.addPass("Bad", [](PassBuilder& b) { b.sample(RGImage{42}); }, [](PassContext&) {}),
                 lumora::rhi::VulkanError);
}

TEST(RenderGraph, ComputePassesDeclareStorageImages) {
    RenderGraph graph;
    const RGImage a = graph.createImage("A", {nvrhi::Format::RGBA16_FLOAT, 8, 8, true});
    graph.addComputePass("Fill", [&](PassBuilder& b) { b.writeStorage(a); }, [](PassContext&) {});
    EXPECT_EQ(graph.passCount(), 1u);
    // More storage images than set 2 has slots is refused when the pass is declared.
    std::vector<RGImage> many;
    for (std::uint32_t i = 0; i <= RenderGraph::kStorageSlots; ++i) many.push_back(graph.createImage("I", {nvrhi::Format::R8_UNORM, 4, 4, true}));
    EXPECT_THROW(graph.addComputePass(
                     "TooMany", [&](PassBuilder& b) { for (RGImage i : many) b.writeStorage(i); }, [](PassContext&) {}),
                 lumora::rhi::VulkanError);
}
