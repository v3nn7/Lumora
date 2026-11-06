#include "assets/MeshLod.hpp"
#include "assets/Primitives.hpp"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <set>

using namespace lumora;

TEST(MeshLod, LevelsShrinkAndStayOnTheSurface) {
    const MeshData sphere = makeUvSphere(1.0f, 128, 64);
    const std::size_t full = sphere.indices.size() / 3;
    const auto lods = buildLods(sphere);
    ASSERT_GE(lods.size(), 2u);
    std::size_t previous = full;
    for (const MeshLod& lod : lods) {
        const std::size_t triangles = lod.indices.size() / 3;
        EXPECT_LE(static_cast<double>(triangles), previous * 0.6 + 1) << "each level clearly smaller";
        EXPECT_GE(triangles, 8u);
        previous = triangles;
        for (std::uint32_t i : lod.indices) {
            ASSERT_LT(i, sphere.vertices.size()) << "indices point into the original vertices";
            EXPECT_NEAR(glm::length(sphere.vertices[i].position), 1.0f, 1e-4f) << "representatives are real vertices";
        }
        for (std::size_t t = 0; t < lod.indices.size(); t += 3) {
            EXPECT_NE(lod.indices[t], lod.indices[t + 1]);
            EXPECT_NE(lod.indices[t + 1], lod.indices[t + 2]);
            EXPECT_NE(lod.indices[t], lod.indices[t + 2]);
        }
    }
    // Front faces still face outwards: the winding survives simplification.
    const MeshLod& coarse = lods.back();
    int outward = 0;
    const int total = static_cast<int>(coarse.indices.size() / 3);
    for (std::size_t t = 0; t < coarse.indices.size(); t += 3) {
        const glm::vec3 a = sphere.vertices[coarse.indices[t]].position;
        const glm::vec3 b = sphere.vertices[coarse.indices[t + 1]].position;
        const glm::vec3 c = sphere.vertices[coarse.indices[t + 2]].position;
        if (glm::dot(glm::cross(b - a, c - a), a + b + c) > 0.0f) ++outward;
    }
    EXPECT_GT(outward, total * 9 / 10);
}

TEST(MeshLod, SmallMeshesGetNoLevelsAndBoxesKeepTheirSides) {
    EXPECT_TRUE(buildLods(makePrimitive(MeshPrimitive::Cube)).empty());
    // Vertices facing different ways never merge: corners shared by three faces stay three
    // vertices, so a box keeps all six sides and every triangle.
    const MeshData cube = makePrimitive(MeshPrimitive::Cube);
    const auto simplified = simplifyByClustering(cube, 0.3f);
    EXPECT_EQ(simplified.size(), cube.indices.size());
    // A cell larger than the whole box collapses every side to a point: nothing is left.
    EXPECT_TRUE(simplifyByClustering(cube, 2.0f).empty());
    std::set<int> facings;
    for (std::size_t t = 0; t < simplified.size(); t += 3) {
        const glm::vec3 n = cube.vertices[simplified[t]].normal;
        facings.insert(static_cast<int>(std::round(n.x)) * 9 + static_cast<int>(std::round(n.y)) * 3 + static_cast<int>(std::round(n.z)));
    }
    EXPECT_EQ(facings.size(), 6u);
}

TEST(MeshLod, SelectionFollowsScreenCoverage) {
    EXPECT_EQ(selectLod(0.5f, 3), 0);
    EXPECT_EQ(selectLod(0.2f, 3), 1);
    EXPECT_EQ(selectLod(0.1f, 3), 2);
    EXPECT_EQ(selectLod(0.01f, 3), 3);
    EXPECT_EQ(selectLod(0.01f, 2), 2) << "clamped to the coarsest level";
    EXPECT_EQ(selectLod(0.01f, 3, 0.0f), 0) << "bias 0 disables";
    EXPECT_EQ(selectLod(0.2f, 3, 2.0f), 0) << "a larger bias keeps detail longer";
    EXPECT_FLOAT_EQ(screenCoverage(1.0f, 0.5f, 1.0f), 1.0f) << "inside the sphere";
    EXPECT_GT(screenCoverage(1.0f, 10.0f, 1.0f), screenCoverage(1.0f, 20.0f, 1.0f));
}
