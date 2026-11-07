#pragma once

#include "scene/Camera.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace lumora::render {

/// Something that casts a sun shadow, as the virtual shadow map tracks it between frames.
struct VsmCaster {
    std::uint64_t key = 0;    // stable identity (entity); several casters may share one
    glm::vec3 center{0.0f};   // world AABB
    glm::vec3 extent{0.0f};
    std::uint64_t state = 0;  // changes whenever its shadow could change (transform, mesh)
    bool dynamic = false;     // animated (skinned): its pages are redrawn every frame
};

/// One page to clear and redraw this frame.
struct VsmPageDraw {
    std::uint32_t physical = 0;  // index in the physical pool (row-major, kPoolPages per row)
    int level = 0;
    glm::vec2 center{0.0f};      // light-space centre of the page (m)
    float invHalf = 1.0f;        // 1 / half its size (m)
    glm::vec2 rectMin{0.0f};     // light-space rectangle (m), for culling casters
    glm::vec2 rectMax{0.0f};
};

struct VsmFrame {
    glm::mat4 lightView{1.0f};  // world -> light space: x, y across the light (m), z along it (m)
    float depthMin = 0.0f;      // depth01 = (z - depthMin) * depthScale
    float depthScale = 1.0f;
    int levels = 0;
    float firstHalf = 0.0f;     // half the size of level 0 (m); level l is twice level l - 1
    float levelReach = 0.0f;    // level l serves view depths up to firstHalf * 2^l * levelReach
    std::vector<VsmPageDraw> render;
    /// RGBA32F texels, kPagesPerLevel wide and kPagesPerLevel * levels high: per page slot of a
    /// level (absolute page mod kPagesPerLevel) the physical page, the absolute page x and y it
    /// holds, and 1 when it is valid.
    std::vector<float> pageTable;
    std::uint32_t requested = 0;  // pages the view needs
    std::uint32_t cached = 0;     // of those, still valid from earlier frames
    bool invalidatedAll = false;
    /// Per level, the lowest absolute page x and y of its window (kPagesPerLevel wide, around
    /// the camera); with `generation`, what GPU page requests of this frame are decoded with.
    std::array<glm::ivec2, 12> windowMin{};
    std::uint64_t generation = 0;  // changes whenever every page is invalidated
    bool fromFeedback = false;     // fine levels requested from the visible surfaces
};

/// Pages the visible surfaces of an earlier frame asked for (read back from the GPU).
struct VsmFeedback {
    std::uint64_t generation = 0;
    std::vector<glm::ivec3> pages;  // level, absolute page x, y
};

/// Virtual shadow map for the sun (in the spirit of Unreal's): a clipmap of square levels, each
/// twice the size of the previous one around the camera, all at the same resolution (16 x 16
/// pages of 128 x 128 texels), so near shadows get millimetre texels and far ones stay cheap.
/// Only the pages the view can see are backed by memory, in a physical pool of 32 x 32 pages
/// (a 4096 x 4096 depth texture), and a page stays valid across frames until its contents could
/// change: the sun turned, or a caster whose light-space rectangle touches it moved, appeared or
/// went away. Animated casters redraw their pages every frame. Pages are addressed by absolute
/// light-space coordinates, so walking back to a place finds its pages still there.
///
/// This class is the CPU side: page requests from the view frustum, the pool and its LRU, the
/// invalidation and the page table. The renderer draws `render` into the pool and samples it
/// through the page table, falling back to coarser levels where a page is missing.
class VirtualShadowMap {
public:
    static constexpr int kPagesPerLevel = 16;
    static constexpr int kPageTexels = 128;
    static constexpr int kPoolPages = 32;  // per side
    static constexpr int kMaxLevels = 12;  // (VsmFrame::windowMin's size)
    static constexpr float kFirstHalf = 4.0f;  // level 0 is 8 m across: 4 mm texels
    [[nodiscard]] static constexpr std::uint32_t poolSize() noexcept { return kPoolPages * kPageTexels; }

    /// Coarsest levels always requested from the view frustum (the fallback for what the GPU
    /// requests have not caught up with: new views, transparent surfaces, fog).
    static constexpr int kFrustumLevels = 2;

    /// `lightTravel` is the direction the sunlight travels. At most `budget` pages are drawn
    /// (coarse levels first); the rest follow in later frames. 0 = no limit. With `feedback`
    /// (of the current generation) the finer levels request only the pages it lists - the ones
    /// visible surfaces sample - instead of everything under the view frustum.
    VsmFrame update(const CameraView& camera, const glm::vec3& lightTravel, float shadowDistance,
                    std::span<const VsmCaster> casters, std::uint32_t budget, const VsmFeedback* feedback = nullptr);
    void invalidateAll();

    /// GPU page requests -> absolute pages. `bits` holds one uint per page slot (non-zero =
    /// requested), kPagesPerLevel wide and kPagesPerLevel * levels high, slots being absolute
    /// pages modulo kPagesPerLevel within the frame's windows.
    [[nodiscard]] static VsmFeedback decodeRequests(std::span<const std::uint32_t> bits, std::size_t rowPitch, int levels,
                                                    const std::array<glm::ivec2, 12>& windowMin, std::uint64_t generation);

    [[nodiscard]] std::size_t residentPages() const noexcept { return resident_.size(); }

private:
    struct Resident {
        std::uint32_t physical = 0;
        int level = 0;
        int x = 0;
        int y = 0;
        bool valid = false;
        std::uint64_t lastUsed = 0;
    };
    struct Tracked {
        std::uint64_t state = 0;
        glm::vec2 min{0.0f};
        glm::vec2 max{0.0f};
        bool dynamic = false;
        std::uint64_t seen = 0;
    };
    void invalidateRect(glm::vec2 min, glm::vec2 max);
    [[nodiscard]] static std::uint64_t pageKey(int level, int x, int y) noexcept;
    [[nodiscard]] static float pageSize(int level) noexcept;

    std::unordered_map<std::uint64_t, Resident> resident_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<std::uint64_t, Tracked> casters_;
    glm::vec3 lightDir_{0.0f};
    glm::vec3 right_{1.0f, 0.0f, 0.0f};
    glm::vec3 up_{0.0f, 1.0f, 0.0f};
    float depthMin_ = 0.0f;
    float depthRange_ = 0.0f;
    std::uint64_t frame_ = 0;
    std::uint64_t generation_ = 0;
    bool initialised_ = false;
};

} // namespace lumora::render
