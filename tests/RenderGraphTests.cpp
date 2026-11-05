#include "renderer/RenderGraph.hpp"

#include <gtest/gtest.h>

using namespace lumora::render;

namespace {
ImportedImage fakeSwapchainImage() {
    ImportedImage img;
    img.image = reinterpret_cast<VkImage>(std::uintptr_t{0x1000});
    img.format = VK_FORMAT_B8G8R8A8_SRGB;
    img.extent = {64, 64};
    img.initialState = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE};
    img.finalUsage = ImageUsage::Present;
    return img;
}
const auto noop = [](PassContext&) {};
} // namespace

TEST(RenderGraph, PlansTransitionsForTypicalFrame) {
    RenderGraph graph;
    const RGImage out = graph.importImage("Swapchain", fakeSwapchainImage());
    const RGImage hdr = graph.createImage("HDR", {VK_FORMAT_R16G16B16A16_SFLOAT, {64, 64}});
    const RGImage depth = graph.createImage("Depth", {VK_FORMAT_D32_SFLOAT, {64, 64}});
    graph.addRasterPass("Opaque", [&](PassBuilder& b) {
        b.writeColor(hdr, LoadOp::Clear);
        b.writeDepth(depth, LoadOp::Clear);
    }, noop);
    graph.addRasterPass("Tonemap", [&](PassBuilder& b) {
        b.sample(hdr);
        b.writeColor(out, LoadOp::DontCare);
    }, noop);

    EXPECT_EQ(graph.requiredUsage(hdr), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    EXPECT_EQ(graph.requiredUsage(depth), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    const auto barriers = graph.planBarriers();
    // Opaque: hdr + depth initial transitions. Tonemap: hdr -> sampled, swapchain -> color. Final: present.
    ASSERT_EQ(barriers.size(), 5u);
    EXPECT_EQ(barriers[0].pass, 0u);
    EXPECT_TRUE(barriers[0].discard);
    EXPECT_EQ(barriers[0].to.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barriers[1].to.layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    EXPECT_EQ(barriers[2].pass, 1u);
    EXPECT_EQ(barriers[2].image, hdr);
    EXPECT_FALSE(barriers[2].discard) << "sampling must preserve the rendered contents";
    EXPECT_EQ(barriers[2].from.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barriers[2].to.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    EXPECT_EQ(barriers[3].image, out);
    EXPECT_EQ(barriers[3].from.stages, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
        << "must chain with the acquire semaphore wait stage";

    EXPECT_EQ(barriers[4].pass, UINT32_MAX);
    EXPECT_EQ(barriers[4].to.layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST(RenderGraph, ReadAfterReadNeedsNoBarrier) {
    RenderGraph graph;
    const RGImage hdr = graph.createImage("HDR", {VK_FORMAT_R16G16B16A16_SFLOAT, {8, 8}});
    graph.addRasterPass("Write", [&](PassBuilder& b) { b.writeColor(hdr, LoadOp::Clear); }, noop);
    graph.addRasterPass("ReadA", [&](PassBuilder& b) { b.sample(hdr); }, noop);
    graph.addRasterPass("ReadB", [&](PassBuilder& b) { b.sample(hdr); }, noop);
    const auto barriers = graph.planBarriers();
    ASSERT_EQ(barriers.size(), 2u);
    EXPECT_EQ(barriers[1].pass, 1u);
}

TEST(RenderGraph, WriteAfterWriteKeepsContentsWhenLoading) {
    RenderGraph graph;
    const RGImage hdr = graph.createImage("HDR", {VK_FORMAT_R16G16B16A16_SFLOAT, {8, 8}});
    graph.addRasterPass("Sky", [&](PassBuilder& b) { b.writeColor(hdr, LoadOp::DontCare); }, noop);
    graph.addRasterPass("Opaque", [&](PassBuilder& b) { b.writeColor(hdr, LoadOp::Load); }, noop);
    const auto barriers = graph.planBarriers();
    ASSERT_EQ(barriers.size(), 2u);
    EXPECT_FALSE(barriers[1].discard);
    EXPECT_TRUE(hasWriteAccess(barriers[1].from.access));
}

TEST(RenderGraph, BarrierMasksFollowHazardType) {
    const ImageState color = stateForUsage(ImageUsage::ColorAttachment);
    const ImageState sampled = stateForUsage(ImageUsage::FragmentSampled);
    auto raw = makeImageBarrier(color, sampled, VK_NULL_HANDLE, VK_IMAGE_ASPECT_COLOR_BIT, false);
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw->srcAccessMask, color.access);
    EXPECT_EQ(raw->dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // Write-after-read: execution dependency only, nothing to make available.
    auto war = makeImageBarrier(sampled, color, VK_NULL_HANDLE, VK_IMAGE_ASPECT_COLOR_BIT, false);
    ASSERT_TRUE(war.has_value());
    EXPECT_EQ(war->srcAccessMask, VK_ACCESS_2_NONE);
    EXPECT_EQ(war->oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_FALSE(makeImageBarrier(sampled, sampled, VK_NULL_HANDLE, VK_IMAGE_ASPECT_COLOR_BIT, false).has_value());
}

TEST(RenderGraph, RejectsInvalidHandles) {
    RenderGraph graph;
    EXPECT_THROW(graph.addRasterPass("Bad", [&](PassBuilder& b) { b.sample(RGImage{42}); }, noop),
                 lumora::rhi::VulkanError);
}
