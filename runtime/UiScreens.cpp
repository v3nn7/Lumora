#include "runtime/UiScreens.hpp"

#include "core/Log.hpp"
#include "input/InputState.hpp"
#include "runtime/GameUi.hpp"
#include "runtime/Localization.hpp"
#include "scene/Hud.hpp"

#include <algorithm>

namespace lumora {

bool UiScreens::open(const std::filesystem::path& projectRoot, const std::string& key) {
    if (const auto it = std::ranges::find(open_, key, &Open::key); it != open_.end()) {
        std::rotate(it, it + 1, open_.end());  // to the front (drawn last)
        return true;
    }
    const std::filesystem::path relative = std::filesystem::path(key).lexically_normal();
    if (key.empty() || relative.is_absolute() || *relative.begin() == "..") {
        log::warn("UI", "'{}' is not a UI document of the project", key);
        return false;
    }
    auto document = loadUiDocument(projectRoot / relative);
    if (!document) {
        log::warn("UI", "{}", document.error().message);
        return false;
    }
    open_.push_back({key, std::move(*document), {}});
    return true;
}

void UiScreens::close(const std::string& key) {
    if (key.empty()) {
        open_.clear();
    } else {
        std::erase_if(open_, [&](const Open& o) { return o.key == key; });
    }
}

bool UiScreens::isOpen(const std::string& key) const { return std::ranges::find(open_, key, &Open::key) != open_.end(); }

UiWidget* UiScreens::widget(std::string_view name) {
    for (auto it = open_.rbegin(); it != open_.rend(); ++it) {
        if (UiWidget* w = findUiWidget(it->document.root, name)) return w;
    }
    return nullptr;
}

void UiScreens::draw(GameUi& ui, HudCanvas& hud, const InputState* input, const Localization* texts) {
    clicked_.clear();
    changed_.clear();
    auto localized = [&](const std::string& text) -> std::string {
        if (texts != nullptr && text.size() > 1 && text.front() == '@') return std::string(texts->text(text.substr(1), text));
        return text;
    };
    const glm::vec2 mouse = ui.mouse();
    const float wheel = input != nullptr ? input->scrollDelta() : 0.0f;
    const float fontSize = ui.style().fontSize;
    for (Open& open : open_) {
        std::map<std::string, float> contentHeight;
        const std::vector<UiPlaced> placed = layoutUi(open.document, hud.screenSize, open.scroll, &contentHeight);
        for (const UiPlaced& p : placed) {
            UiWidget& w = *uiWidgetAt(open.document.root, p.path);
            const glm::vec4 r = p.rect;
            // Cut to scrolled panels; widgets scrolled out of sight take no input.
            const bool unclipped = p.clip == HudCanvas::kNoClip;
            if (!unclipped && (r.x >= p.clip.z || r.y >= p.clip.w || r.x + r.z <= p.clip.x || r.y + r.w <= p.clip.y)) continue;
            hud.setClip(p.clip);
            const std::string text = localized(w.text);
            ui.style().fontSize = w.fontSize;
            switch (w.type) {
                case UiWidget::Type::Panel: {
                    if (w.color.a > 0.0f) hud.addRect({r.x, r.y}, {r.z, r.w}, w.color);
                    if (!w.scroll) break;
                    // Mouse wheel over it scrolls, within its content; a thin bar shows where.
                    const float content = contentHeight[p.path];
                    const float view = std::max(r.w - w.padding.y - w.padding.w, 1.0f);
                    float& offset = open.scroll[p.path];
                    const bool over = mouse.x >= r.x && mouse.y >= r.y && mouse.x < r.x + r.z && mouse.y < r.y + r.w;
                    if (over && wheel != 0.0f) offset -= wheel * 40.0f;
                    offset = std::clamp(offset, 0.0f, std::max(content - view, 0.0f));
                    if (content > view) {
                        const float bar = std::max(view * view / content, 16.0f);
                        const float at = (view - bar) * (offset / (content - view));
                        hud.addRect({r.x + r.z - 5.0f, r.y + w.padding.y + at}, {3.0f, bar}, {1.0f, 1.0f, 1.0f, 0.35f});
                    }
                    break;
                }
                case UiWidget::Type::Label: {
                    const float y = r.y + (r.w - w.fontSize * 1.15f) * 0.5f;
                    hud.addText({r.x, y}, w.fontSize, w.color, text, static_cast<HudCanvas::Align>(w.align), r.z, w.font);
                    break;
                }
                case UiWidget::Type::Button:
                    if (ui.button(text, r) && !w.name.empty()) clicked_.insert(w.name);
                    break;
                case UiWidget::Type::Checkbox:
                    if (ui.checkbox(text, w.value != 0.0f, r)) {
                        w.value = w.value != 0.0f ? 0.0f : 1.0f;
                        if (!w.name.empty()) {
                            clicked_.insert(w.name);
                            changed_.insert(w.name);
                        }
                    }
                    break;
                case UiWidget::Type::Slider:
                    if (ui.slider(text, w.value, w.min, w.max, r) && !w.name.empty()) changed_.insert(w.name);
                    break;
                case UiWidget::Type::Image:
                    if (!w.image.empty()) hud.addImage({r.x, r.y}, {r.z, r.w}, w.image, w.color);
                    break;
            }
        }
    }
    ui.style().fontSize = fontSize;
    hud.clearClip();
}

} // namespace lumora
