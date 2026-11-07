#include "GltfTestUtils.hpp"
#include "TestUtils.hpp"

#include "assets/AssetLibrary.hpp"
#include "assets/GltfImporter.hpp"
#include "assets/Image.hpp"
#include "core/FileSystem.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace lumora;

namespace {

// 4x2 lossless WebP (encoded with libwebp): red, green, blue, half-transparent white; then
// (10,20,30), fully transparent (200,100,50), black, (77,88,99,200).
constexpr std::array<std::uint8_t, 86> kWebp{
    0x52, 0x49, 0x46, 0x46, 0x4e, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c, 0x42, 0x00,
    0x00, 0x00, 0x2f, 0x03, 0x40, 0x00, 0x10, 0x3f, 0x40, 0x16, 0x60, 0xb2, 0x20, 0xf2, 0xca, 0x9f, 0xa3, 0x7f,
    0x1a, 0x64, 0xc2, 0x10, 0xfb, 0xe5, 0x56, 0x4b, 0x35, 0x64, 0x02, 0x16, 0xd3, 0xc1, 0x2f, 0x83, 0xfe, 0xe1,
    0x46, 0x20, 0x69, 0xb3, 0x25, 0x3f, 0x6f, 0xf7, 0x56, 0x0f, 0x70, 0x88, 0x42, 0x0e, 0xb2, 0xcd, 0xb2, 0xd5,
    0xf7, 0x83, 0x50, 0x00, 0x8d, 0xc1, 0x44, 0x12, 0x4c, 0xbf, 0x88, 0xfe, 0x07, 0x36};
constexpr std::array<std::array<std::uint8_t, 4>, 8> kWebpPixels{{{255, 0, 0, 255},
                                                                   {0, 255, 0, 255},
                                                                   {0, 0, 255, 255},
                                                                   {255, 255, 255, 128},
                                                                   {10, 20, 30, 255},
                                                                   {200, 100, 50, 0},
                                                                   {0, 0, 0, 255},
                                                                   {77, 88, 99, 200}}};

std::span<const std::byte> bytesOf(std::span<const std::uint8_t> data) { return std::as_bytes(data); }

void writeBytes(const std::filesystem::path& file, std::span<const std::uint8_t> data) {
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

/// An uncompressed single-part scanline OpenEXR file with FLOAT B, G, R channels, written by
/// hand from the specification (independent of the reader under test).
std::vector<std::uint8_t> makeExr(int width, int height, const std::vector<float>& rgb) {
    std::vector<std::uint8_t> out;
    auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto i32 = [&](std::int32_t v) { put(&v, 4); };
    auto f32 = [&](float v) { put(&v, 4); };
    auto str = [&](const char* s) { put(s, std::strlen(s) + 1); };
    auto attribute = [&](const char* name, const char* type, std::int32_t size) {
        str(name);
        str(type);
        i32(size);
    };
    const std::uint8_t magic[] = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
    put(magic, sizeof magic);
    attribute("channels", "chlist", 3 * (2 + 16) + 1);
    for (const char* channel : {"B", "G", "R"}) {
        str(channel);
        i32(2);  // FLOAT
        const std::uint8_t linearAndReserved[4] = {0, 0, 0, 0};
        put(linearAndReserved, 4);
        i32(1);
        i32(1);
    }
    out.push_back(0);
    attribute("compression", "compression", 1);
    out.push_back(0);  // NO_COMPRESSION
    for (const char* window : {"dataWindow", "displayWindow"}) {
        attribute(window, "box2i", 16);
        i32(0);
        i32(0);
        i32(width - 1);
        i32(height - 1);
    }
    attribute("lineOrder", "lineOrder", 1);
    out.push_back(0);  // INCREASING_Y
    attribute("pixelAspectRatio", "float", 4);
    f32(1.0f);
    attribute("screenWindowCenter", "v2f", 8);
    f32(0.0f);
    f32(0.0f);
    attribute("screenWindowWidth", "float", 4);
    f32(1.0f);
    out.push_back(0);  // end of header
    const std::size_t lineBytes = static_cast<std::size_t>(width) * 3 * 4;
    const std::size_t tableStart = out.size();
    for (int y = 0; y < height; ++y) {
        const std::uint64_t offset = tableStart + static_cast<std::size_t>(height) * 8 + static_cast<std::size_t>(y) * (8 + lineBytes);
        put(&offset, 8);
    }
    for (int y = 0; y < height; ++y) {
        i32(y);
        i32(static_cast<std::int32_t>(lineBytes));
        for (int channel : {2, 1, 0}) {  // B, G, R
            for (int x = 0; x < width; ++x) f32(rgb[(static_cast<std::size_t>(y) * width + x) * 3 + channel]);
        }
    }
    return out;
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

/// The cube glTF with a textured material whose only image comes from a texture extension.
std::filesystem::path writeExtensionTexturedCube(const std::filesystem::path& dir, const char* extension, const char* mime,
                                                 const std::vector<unsigned char>& encoded) {
    const std::filesystem::path file = dir / "cube.gltf";
    test::writeCubeGltf(file, false, true);
    std::string json = readText(file);
    const std::size_t texturesAt = json.find(R"("textures": [{"source": 0}],)");
    const std::size_t imagesAt = json.find(R"("images": [)");
    const std::size_t imagesEnd = json.find("}],", imagesAt) + 3;
    EXPECT_NE(texturesAt, std::string::npos);
    EXPECT_NE(imagesAt, std::string::npos);
    const std::string images = std::format(R"("images": [{{"name": "Ext", "uri": "data:{};base64,{}"}}],)", mime, test::base64(encoded));
    json.replace(imagesAt, imagesEnd - imagesAt, images);
    json.replace(texturesAt, std::strlen(R"("textures": [{"source": 0}],)"),
                 std::format(R"("textures": [{{"extensions": {{"{}": {{"source": 0}}}}}}],)", extension));
    json.insert(json.find('{') + 1, std::format(R"("extensionsUsed": ["{}"], "extensionsRequired": ["{}"],)", extension, extension));
    std::ofstream(file) << json;
    return file;
}

} // namespace

TEST(ImageFormats, DecodesLosslessWebpWithAlpha) {
    auto image = decodeImage(bytesOf(kWebp));
    ASSERT_TRUE(image.has_value()) << image.error().message;
    ASSERT_EQ(image->width, 4u);
    ASSERT_EQ(image->height, 2u);
    for (std::size_t i = 0; i < kWebpPixels.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (c < 3 && kWebpPixels[i][3] == 0) continue;  // colour under zero alpha is not kept exactly
            EXPECT_EQ(image->rgba[i * 4 + c], kWebpPixels[i][c]) << "pixel " << i << " channel " << c;
        }
    }
    std::array<std::uint8_t, 40> truncated{};
    std::memcpy(truncated.data(), kWebp.data(), truncated.size());
    EXPECT_FALSE(decodeImage(bytesOf(truncated)).has_value());
    EXPECT_TRUE(isImageFile("a/b/Leaves.WEBP"));
    EXPECT_TRUE(isImageFile("bark.ktx2"));
}

TEST(ImageFormats, LoadsOpenExrEnvironments) {
    test::TempDir dir;
    const int w = 3, h = 2;
    std::vector<float> rgb(static_cast<std::size_t>(w * h * 3));
    for (std::size_t i = 0; i < rgb.size(); ++i) rgb[i] = 0.25f * static_cast<float>(i) + 0.5f;
    rgb[4] = 60000.0f;  // HDR: far above 1
    const auto exr = makeExr(w, h, rgb);
    writeBytes(dir.path() / "sky.exr", exr);
    EXPECT_TRUE(isHdrFile(dir.path() / "sky.EXR"));
    auto image = loadHdrFile(dir.path() / "sky.exr");
    ASSERT_TRUE(image.has_value()) << image.error().message;
    ASSERT_EQ(image->width, 3u);
    ASSERT_EQ(image->height, 2u);
    for (std::size_t i = 0; i < rgb.size(); ++i) EXPECT_FLOAT_EQ(image->rgb[i], rgb[i]) << i;

    std::vector<std::uint8_t> broken(exr.begin(), exr.begin() + static_cast<std::ptrdiff_t>(exr.size() / 2));
    writeBytes(dir.path() / "broken.exr", broken);
    EXPECT_FALSE(loadHdrFile(dir.path() / "broken.exr").has_value());
}

TEST(ImageFormats, TranscodesBasisKtx2) {
    // kodim23 from Basis Universal's test files: ETC1S in KTX2 and the original PNG.
    const std::filesystem::path dir = LUMORA_TEST_BASISU_DIR;
    auto ktx2Bytes = fs::readBinaryFile(dir / "test_files" / "kodim23.ktx2");
    auto pngBytes = fs::readBinaryFile(dir / "test_files" / "kodim23.png");
    ASSERT_TRUE(ktx2Bytes.has_value() && pngBytes.has_value());
    auto ktx2 = decodeImage(*ktx2Bytes);
    auto png = decodeImage(*pngBytes);
    ASSERT_TRUE(ktx2.has_value()) << ktx2.error().message;
    ASSERT_TRUE(png.has_value()) << png.error().message;
    ASSERT_EQ(ktx2->width, png->width);
    ASSERT_EQ(ktx2->height, png->height);
    double squared = 0.0;
    for (std::size_t i = 0; i < png->rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = static_cast<double>(ktx2->rgba[i + c]) - static_cast<double>(png->rgba[i + c]);
            squared += d * d;
        }
        EXPECT_EQ(ktx2->rgba[i + 3], 255);
    }
    const double mse = squared / static_cast<double>(png->rgba.size() / 4 * 3);
    const double psnr = 10.0 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9));
    EXPECT_GT(psnr, 28.0) << "the transcoded image matches the original (ETC1S quality)";

    std::vector<std::byte> header(ktx2Bytes->begin(), ktx2Bytes->begin() + 200);
    EXPECT_FALSE(decodeImage(header).has_value()) << "truncated KTX2";
}

TEST(ImageFormats, GltfTextureExtensionsProvideTheImage) {
    test::TempDir dir;
    const std::vector<unsigned char> webp(kWebp.begin(), kWebp.end());
    const auto file = writeExtensionTexturedCube(dir.path(), "EXT_texture_webp", "image/webp", webp);
    auto model = importGltf(file);
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ(model->images.size(), 1u);
    EXPECT_EQ(model->materials[0].baseColorTexture, 0);
    auto decoded = decodeImage(model->images[0].encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width, 4u);

    auto ktx2Bytes = fs::readBinaryFile(std::filesystem::path(LUMORA_TEST_BASISU_DIR) / "test_files" / "kodim23.ktx2");
    ASSERT_TRUE(ktx2Bytes.has_value());
    std::vector<unsigned char> ktx2(ktx2Bytes->size());
    std::memcpy(ktx2.data(), ktx2Bytes->data(), ktx2.size());
    std::filesystem::create_directories(dir.path() / "basis");
    const auto basisFile = writeExtensionTexturedCube(dir.path() / "basis", "KHR_texture_basisu", "image/ktx2", ktx2);
    auto basis = importGltf(basisFile);
    ASSERT_TRUE(basis.has_value()) << basis.error().message;
    EXPECT_EQ(basis->materials[0].baseColorTexture, 0);
    for (const std::string& warning : basis->warnings) EXPECT_EQ(warning.find("texture"), std::string::npos) << warning;
    AssetLibrary library(dir.path());
    auto texture = library.texture("basis/cube.gltf#tex0");
    ASSERT_TRUE(texture.has_value()) << texture.error().message;
    EXPECT_EQ((*texture)->width, 768u);
}
