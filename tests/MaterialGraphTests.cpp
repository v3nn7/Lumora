#include "TestUtils.hpp"

#include "assets/Material.hpp"
#include "assets/MaterialGraph.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <fstream>
#include <set>

using namespace lumora;

namespace {

/// Builds graph documents as GraphEditorPanel saves them.
struct GraphBuilder {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json links = nlohmann::json::array();
    int next = 1;
    int add(std::string_view type, nlohmann::json inputs = nlohmann::json::object()) {
        nodes.push_back({{"id", next}, {"type", std::string(type)}, {"x", 0}, {"y", 0}, {"inputs", std::move(inputs)}});
        return next++;
    }
    void link(int from, std::string_view fromPin, int to, std::string_view toPin) {
        links.push_back({{"from", from}, {"fromPin", std::string(fromPin)}, {"to", to}, {"toPin", std::string(toPin)}});
    }
    [[nodiscard]] nlohmann::json document() const { return {{"version", 1}, {"nodes", nodes}, {"links", links}}; }
};

int opAt(const std::vector<glm::vec4>& code, std::size_t instruction) { return static_cast<int>(code[instruction * 2].x); }

} // namespace

TEST(MaterialGraph, DefaultGraphWritesEveryOutput) {
    auto program = compileMaterialGraph(defaultMaterialGraph());
    ASSERT_TRUE(program.has_value()) << program.error().message;
    EXPECT_EQ(program->fragmentInstructions(), 7u) << "a constant per output";
    EXPECT_TRUE(program->vertex.empty()) << "no world offset linked: no vertex program";
    EXPECT_EQ(program->alphaMode, 0);
    for (std::size_t i = 0; i < 7; ++i) EXPECT_EQ(opAt(program->fragment, i), static_cast<int>(matgraph::Op::Const));
    EXPECT_FLOAT_EQ(program->fragment[1].x, 0.8f) << "base colour default";
    EXPECT_FLOAT_EQ(program->fragment[9].z, 1.0f) << "the normal points out of the surface";
}

TEST(MaterialGraph, TextureTimesColourAndOutputSettings) {
    GraphBuilder g;
    const int out = g.add("output.material", {{"alphaMode", 1}, {"alphaCutoff", 0.3}, {"twoSided", true}, {"roughness", 0.6}});
    const int tex = g.add("texture.color", {{"texture", "Content/Textures/a.png"}});
    const int tint = g.add("const.color", {{"color", {1.0, 0.5, 0.25}}});
    const int mul = g.add("math.multiply");
    g.link(tex, "rgb", mul, "a");
    g.link(tint, "color", mul, "b");
    g.link(mul, "result", out, "baseColor");
    g.link(tex, "a", out, "opacity");
    auto program = compileMaterialGraph(g.document());
    ASSERT_TRUE(program.has_value()) << program.error().message;
    ASSERT_EQ(program->textures.size(), 1u);
    EXPECT_EQ(program->textures[0].key, "Content/Textures/a.png");
    EXPECT_EQ(program->textures[0].kind, matgraph::TextureKind::Color);
    EXPECT_EQ(program->alphaMode, 1);
    EXPECT_FLOAT_EQ(program->alphaCutoff, 0.3f);
    EXPECT_TRUE(program->twoSided);
    std::set<int> ops;
    for (std::size_t i = 0; i < program->fragmentInstructions(); ++i) ops.insert(opAt(program->fragment, i));
    for (matgraph::Op op : {matgraph::Op::Texture, matgraph::Op::Multiply, matgraph::Op::Splat, matgraph::Op::Input}) {
        EXPECT_TRUE(ops.contains(static_cast<int>(op))) << static_cast<int>(op);
    }
    // Every register the program names is in range.
    for (std::size_t i = 0; i < program->fragmentInstructions(); ++i) {
        const glm::vec4 a = program->fragment[i * 2];
        for (int c = 1; c < 4; ++c) EXPECT_LT(a[c], static_cast<float>(matgraph::kRegisters));
    }
}

TEST(MaterialGraph, LongChainsReuseRegistersAndWindMakesAVertexProgram) {
    GraphBuilder g;
    const int out = g.add("output.material");
    int previous = g.add("input.uv");
    std::string pin = "uv";
    for (int i = 0; i < 60; ++i) {
        const int add = g.add("math.add", {{"b", 0.01}});
        g.link(previous, pin, add, "a");
        previous = add;
        pin = "result";
    }
    g.link(previous, pin, out, "baseColor");
    const int wind = g.add("wind.sway", {{"strength", 0.2}});
    const int height = g.add("input.objectPosition");
    const int split = g.add("vector.split");
    g.link(height, "position", split, "vector");
    g.link(split, "y", wind, "mask");
    g.link(wind, "offset", out, "worldOffset");
    auto program = compileMaterialGraph(g.document());
    ASSERT_TRUE(program.has_value()) << program.error().message;
    EXPECT_GT(program->fragmentInstructions(), 60u);
    ASSERT_FALSE(program->vertex.empty());
    EXPECT_EQ(opAt(program->vertex, program->vertexInstructions() - 1), static_cast<int>(matgraph::Op::Move));
    EXPECT_FLOAT_EQ(program->vertex.back().x, 0.0f);
    EXPECT_FLOAT_EQ(program->vertex[(program->vertexInstructions() - 1) * 2].y, static_cast<float>(matgraph::kWorldOffset));
}

TEST(MaterialGraph, BrokenGraphsAreErrors) {
    EXPECT_FALSE(compileMaterialGraph(nlohmann::json::array()).has_value());
    GraphBuilder none;
    none.add("const.number");
    EXPECT_FALSE(compileMaterialGraph(none.document()).has_value()) << "no output";
    GraphBuilder two;
    two.add("output.material");
    two.add("output.material");
    EXPECT_FALSE(compileMaterialGraph(two.document()).has_value()) << "two outputs";
    GraphBuilder unknown;
    unknown.add("output.material");
    unknown.add("no.such.node");
    EXPECT_FALSE(compileMaterialGraph(unknown.document()).has_value());
    GraphBuilder cycle;
    const int out = cycle.add("output.material");
    const int a = cycle.add("math.add");
    const int b = cycle.add("math.add");
    cycle.link(a, "result", b, "a");
    cycle.link(b, "result", a, "a");
    cycle.link(b, "result", out, "baseColor");
    EXPECT_FALSE(compileMaterialGraph(cycle.document()).has_value()) << "a cycle";
    GraphBuilder textures;
    const int o = textures.add("output.material");
    int sum = textures.add("const.color");
    for (int i = 0; i < 9; ++i) {
        const int t = textures.add("texture.color", {{"texture", std::format("Content/Textures/{}.png", i)}});
        const int add = textures.add("math.add");
        textures.link(sum, sum == 2 ? "color" : "result", add, "a");
        textures.link(t, "rgb", add, "b");
        sum = add;
    }
    textures.link(sum, "result", o, "baseColor");
    auto many = compileMaterialGraph(textures.document());
    EXPECT_FALSE(many.has_value()) << "nine textures";
}

TEST(MaterialGraph, LoadsAsAMaterialAsset) {
    test::TempDir dir;
    GraphBuilder g;
    g.add("output.material", {{"alphaMode", 2}, {"twoSided", true}});
    std::ofstream(dir.path() / "Glass.lmgraph") << g.document().dump(2);
    auto material = loadMaterial(dir.path() / "Glass.lmgraph");
    ASSERT_TRUE(material.has_value()) << material.error().message;
    ASSERT_NE(material->program, nullptr);
    EXPECT_EQ(material->alphaMode, MaterialAsset::AlphaMode::Transparent);
    EXPECT_TRUE(material->doubleSided);
    std::ofstream(dir.path() / "Broken.lmgraph") << "{\"nodes\": []}";
    EXPECT_FALSE(loadMaterial(dir.path() / "Broken.lmgraph").has_value());
}
