#include "editor/SceneIcons.hpp"

#include "scene/Components.hpp"
#include "scene/Scene.hpp"

#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace lumora::editor {

namespace {

constexpr const char* kFiles[] = {"badge", "badge-selected", "sun", "bulb", "spot", "camera", "speaker", "sparkles"};
static_assert(std::size(kFiles) == static_cast<std::size_t>(SceneIcons::Picture::Count));
constexpr int kPadding = 2;  // texels between pictures so filtering never reaches a neighbour

glm::vec4 rgb(int hex, float alpha = 1.0f) {
    return {((hex >> 16) & 0xff) / 255.0f, ((hex >> 8) & 0xff) / 255.0f, (hex & 0xff) / 255.0f, alpha};
}

/// A light's colour as a readable tint: full brightness, a little towards white.
glm::vec4 lightTint(const glm::vec3& color) {
    const float peak = std::max({color.r, color.g, color.b, 1e-4f});
    return glm::vec4(glm::mix(color / peak, glm::vec3(1.0f), 0.25f), 1.0f);
}

/// Transparent texels take the colour of an opaque neighbour, so bilinear filtering at a
/// picture's edge blends towards its own colour instead of black.
void bleedColour(QImage& image) {
    for (int pass = 0; pass < 3; ++pass) {
        const QImage source = image.copy();
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<std::uint8_t*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                if (row[x * 4 + 3] != 0) continue;
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= image.width() || sy >= image.height()) continue;
                        const auto* s = reinterpret_cast<const std::uint8_t*>(source.constScanLine(sy)) + sx * 4;
                        if (s[3] == 0 && (s[0] | s[1] | s[2]) == 0) continue;
                        r += s[0], g += s[1], b += s[2], ++n;
                    }
                }
                if (n > 0) row[x * 4] = r / n, row[x * 4 + 1] = g / n, row[x * 4 + 2] = b / n;
            }
        }
    }
}

} // namespace

void SceneIcons::rasterize(float pixelRatio) {
    // Drawn at the size they appear on screen, so they stay sharp.
    const int cell = std::max(8, static_cast<int>(std::lround(size_ * pixelRatio)));
    const int count = static_cast<int>(Picture::Count);
    const int columns = 4;
    const int rows = (count + columns - 1) / columns;
    const int stride = cell + 2 * kPadding;
    QImage image(columns * stride, rows * stride, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < count; ++i) {
            const QRectF where((i % columns) * stride + kPadding, (i / columns) * stride + kPadding, cell, cell);
            QSvgRenderer(QStringLiteral(":/icons/scene/%1.svg").arg(QLatin1String(kFiles[i]))).render(&painter, where);
            uv_[i] = {where.left() / image.width(), where.top() / image.height(), where.right() / image.width(),
                      where.bottom() / image.height()};
        }
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    bleedColour(image);
    atlas_.width = static_cast<std::uint32_t>(image.width());
    atlas_.height = static_cast<std::uint32_t>(image.height());
    atlas_.rgba.assign(image.constBits(), image.constBits() + image.sizeInBytes());
    ++atlas_.version;
    atlasRatio_ = pixelRatio;
}

void SceneIcons::build(const Scene& scene, const glm::vec3& eye, const std::vector<entt::entity>& selected,
                       float pixelRatio, render::SpriteBatch& out) {
    if (atlasRatio_ != pixelRatio || atlas_.rgba.empty()) rasterize(pixelRatio);
    out.atlas = &atlas_;
    out.pixelRatio = pixelRatio;
    const auto& reg = scene.registry();
    for (auto [e, world] : reg.view<WorldTransformComponent>().each()) {
        Picture picture = Picture::Count;
        glm::vec4 tint{1.0f};
        if (const auto* light = reg.try_get<LightComponent>(e)) {
            picture = light->type == LightType::Directional ? Picture::Sun
                      : light->type == LightType::Spot     ? Picture::Spot
                                                           : Picture::Bulb;
            tint = lightTint(light->color);
        } else if (reg.all_of<CameraComponent>(e)) {
            picture = Picture::Camera;
            tint = rgb(0x9cc2ff);
        } else if (reg.all_of<AudioSourceComponent>(e)) {
            picture = Picture::Speaker;
            tint = rgb(0x7fe0a8);
        } else if (reg.all_of<ParticleEmitterComponent>(e)) {
            picture = Picture::Sparkles;
            tint = rgb(0xffbd73);
        }
        if (picture == Picture::Count) continue;
        const glm::vec3 position(world.matrix[3]);
        // Standing in it: fade out instead of filling the view.
        const float fade = std::clamp((glm::distance(eye, position) - 0.4f) / 0.8f, 0.0f, 1.0f);
        if (fade <= 0.0f) continue;
        const bool isSelected = std::ranges::find(selected, e) != selected.end();
        const auto id = static_cast<std::uint32_t>(e);
        const Picture badge = isSelected ? Picture::BadgeSelected : Picture::Badge;
        tint.a *= fade;
        out.sprites.push_back({position, size_, uv_[static_cast<int>(badge)], glm::vec4(1.0f, 1.0f, 1.0f, fade), id});
        out.sprites.push_back({position, size_, uv_[static_cast<int>(picture)], tint, id});
    }
}

entt::entity SceneIcons::pick(const render::SpriteBatch& icons, const CameraView& camera, glm::vec2 targetPixels,
                              glm::vec2 pixel) {
    const render::Sprite* hit = render::spriteAt(icons, camera, targetPixels, pixel);
    return hit != nullptr ? static_cast<entt::entity>(hit->id) : entt::entity{entt::null};
}

} // namespace lumora::editor
