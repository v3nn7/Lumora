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
