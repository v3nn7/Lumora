#include "assets/Primitives.hpp"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

using namespace lumora;

class PrimitiveTest : public ::testing::TestWithParam<MeshPrimitive> {};

TEST_P(PrimitiveTest, IndicesAreValidTriangles) {
    const MeshData mesh = makePrimitive(GetParam());
    ASSERT_FALSE(mesh.vertices.empty());
    ASSERT_EQ(mesh.indices.size() % 3, 0u);
    for (std::uint32_t i : mesh.indices) {
        ASSERT_LT(i, mesh.vertices.size());
    }
}

TEST_P(PrimitiveTest, NormalsAreUnitLength) {
    const MeshData mesh = makePrimitive(GetParam());
    for (const Vertex& v : mesh.vertices) {
        EXPECT_NEAR(glm::length(v.normal), 1.0f, 1e-4f);
    }
}

TEST_P(PrimitiveTest, WindingIsCounterClockwiseFromOutside) {
    // Geometric normal from CCW winding must agree with the shading normal.
    const MeshData mesh = makePrimitive(GetParam());
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        const Vertex& a = mesh.vertices[mesh.indices[t]];
        const Vertex& b = mesh.vertices[mesh.indices[t + 1]];
        const Vertex& c = mesh.vertices[mesh.indices[t + 2]];
        const glm::vec3 geometric = glm::cross(b.position - a.position, c.position - a.position);
        if (glm::length(geometric) < 1e-9f) {
            ADD_FAILURE() << "degenerate triangle " << t / 3;
            continue;
        }
        const glm::vec3 shading = a.normal + b.normal + c.normal;
        EXPECT_GT(glm::dot(geometric, shading), 0.0f) << "triangle " << t / 3 << " is wound clockwise";
    }
}

TEST_P(PrimitiveTest, BoundsContainAllVertices) {
    const MeshData mesh = makePrimitive(GetParam());
    for (const Vertex& v : mesh.vertices) {
        for (int i = 0; i < 3; ++i) {
            EXPECT_GE(v.position[i], mesh.bounds.min[i]);
            EXPECT_LE(v.position[i], mesh.bounds.max[i]);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AllPrimitives, PrimitiveTest,
                         ::testing::Values(MeshPrimitive::Cube, MeshPrimitive::Sphere, MeshPrimitive::Plane,
                                           MeshPrimitive::Cylinder),
                         [](const auto& p) { return std::string(toString(p.param)); });

TEST(Primitives, CubeHasExpectedTopology) {
    const MeshData cube = makeCube(2.0f);
    EXPECT_EQ(cube.vertices.size(), 24u);
    EXPECT_EQ(cube.indices.size(), 36u);
    EXPECT_FLOAT_EQ(cube.bounds.min.x, -1.0f);
    EXPECT_FLOAT_EQ(cube.bounds.max.y, 1.0f);
}

TEST_P(PrimitiveTest, TangentsAreUnitAndOrthogonalToNormals) {
    const MeshData mesh = makePrimitive(GetParam());
    for (const Vertex& v : mesh.vertices) {
        const glm::vec3 t(v.tangent);
        EXPECT_NEAR(glm::length(t), 1.0f, 1e-3f);
        EXPECT_NEAR(glm::dot(t, v.normal), 0.0f, 1e-3f);
        EXPECT_TRUE(v.tangent.w == 1.0f || v.tangent.w == -1.0f);
    }
}

TEST(Primitives, PlaneTangentFollowsU) {
    MeshData plane = makePlane(2.0f, 1);
    generateTangents(plane);
    // u increases along +X on the plane.
    EXPECT_NEAR(plane.vertices[0].tangent.x, 1.0f, 1e-5f);
}
