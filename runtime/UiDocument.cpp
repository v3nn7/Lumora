#include "runtime/UiDocument.hpp"

#include "core/FileSystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>

namespace lumora {

namespace {

constexpr std::pair<UiWidget::Type, std::string_view> kTypes[] = {
    {UiWidget::Type::Panel, "panel"},       {UiWidget::Type::Label, "label"},   {UiWidget::Type::Button, "button"},
    {UiWidget::Type::Checkbox, "checkbox"}, {UiWidget::Type::Slider, "slider"}, {UiWidget::Type::Image, "image"}};
constexpr std::pair<UiWidget::Layout, std::string_view> kLayouts[] = {{UiWidget::Layout::None, "none"},
                                                                      {UiWidget::Layout::Vertical, "vertical"},
                                                                      {UiWidget::Layout::Horizontal, "horizontal"},
                                                                      {UiWidget::Layout::Grid, "grid"}};

template <class V>
nlohmann::json array(const V& v) {
    nlohmann::json a = nlohmann::json::array();
    for (int i = 0; i < V::length(); ++i) a.push_back(v[i]);
    return a;
}

nlohmann::json widgetToJson(const UiWidget& w) {
    nlohmann::json j{{"type", std::string(uiTypeName(w.type))},
                     {"name", w.name},
                     {"anchorMin", array(w.anchorMin)},
                     {"anchorMax", array(w.anchorMax)},
                     {"offsetMin", array(w.offsetMin)},
                     {"offsetMax", array(w.offsetMax)},
                     {"size", array(w.size)},
                     {"visible", w.visible},
                     {"color", array(w.color)},
                     {"text", w.text},
                     {"fontSize", w.fontSize},
                     {"font", w.font},
                     {"align", w.align},
                     {"image", w.image},
                     {"value", w.value},
                     {"min", w.min},
                     {"max", w.max}};
    if (w.type == UiWidget::Type::Panel) {
        for (const auto& [layout, name] : kLayouts) {
            if (layout == w.layout) j["layout"] = std::string(name);
        }
        j["columns"] = w.columns;
        j["spacing"] = w.spacing;
        j["padding"] = array(w.padding);
        j["scroll"] = w.scroll;
        nlohmann::json children = nlohmann::json::array();
        for (const UiWidget& c : w.children) children.push_back(widgetToJson(c));
        j["children"] = std::move(children);
    }
    return j;
}

template <class V>
Result<> readVector(const nlohmann::json& j, const char* key, V& out) {
    if (!j.contains(key)) return {};
    const auto& a = j[key];
    if (!a.is_array() || a.size() != static_cast<std::size_t>(V::length())) return makeError("'{}' must be a list of {} numbers", key, V::length());
    for (int i = 0; i < V::length(); ++i) {
        if (!a[static_cast<std::size_t>(i)].is_number()) return makeError("'{}' must be numbers", key);
        out[i] = a[static_cast<std::size_t>(i)].get<float>();
    }
    return {};
}

Result<UiWidget> widgetFromJson(const nlohmann::json& j, int depth) {
    if (!j.is_object()) return makeError("a widget must be a JSON object");
    if (depth > 64) return makeError("widgets nested too deeply");
    UiWidget w;
    const std::string type = j.value("type", std::string("panel"));
    bool known = false;
    for (const auto& [t, name] : kTypes) {
        if (name == type) {
            w.type = t;
            known = true;
        }
    }
    if (!known) return makeError("unknown widget type '{}'", type);
    auto text = [&](const char* key, std::string& out) -> Result<> {
        if (!j.contains(key)) return {};
        if (!j[key].is_string()) return makeError("'{}' must be text", key);
        out = j[key].get<std::string>();
        return {};
    };
    auto number = [&](const char* key, auto& out) -> Result<> {
        if (!j.contains(key)) return {};
        if (!j[key].is_number()) return makeError("'{}' must be a number", key);
        out = j[key].get<std::remove_reference_t<decltype(out)>>();
        return {};
    };
    auto flag = [&](const char* key, bool& out) -> Result<> {
        if (!j.contains(key)) return {};
        if (!j[key].is_boolean()) return makeError("'{}' must be true or false", key);
        out = j[key].get<bool>();
        return {};
    };
    std::string layout = "none";
    for (const Result<>& r : {text("name", w.name), readVector(j, "anchorMin", w.anchorMin), readVector(j, "anchorMax", w.anchorMax),
                              readVector(j, "offsetMin", w.offsetMin), readVector(j, "offsetMax", w.offsetMax),
                              readVector(j, "size", w.size), flag("visible", w.visible), readVector(j, "color", w.color),
                              text("text", w.text), number("fontSize", w.fontSize), text("font", w.font), number("align", w.align),
                              text("image", w.image), number("value", w.value), number("min", w.min), number("max", w.max),
                              text("layout", layout), number("columns", w.columns), number("spacing", w.spacing),
                              readVector(j, "padding", w.padding), flag("scroll", w.scroll)}) {
        if (!r) return makeError("{} '{}': {}", type, w.name, r.error().message);
    }
    bool layoutKnown = false;
    for (const auto& [l, name] : kLayouts) {
        if (name == layout) {
            w.layout = l;
            layoutKnown = true;
        }
    }
    if (!layoutKnown) return makeError("{} '{}': unknown layout '{}'", type, w.name, layout);
    w.columns = std::max(w.columns, 1);
    w.align = std::clamp(w.align, 0, 2);
    if (j.contains("children")) {
        if (!j["children"].is_array()) return makeError("{} '{}': 'children' must be a list", type, w.name);
        for (const auto& c : j["children"]) {
            auto child = widgetFromJson(c, depth + 1);
            if (!child) return std::unexpected(child.error());
            w.children.push_back(std::move(*child));
        }
    }
    return w;
}

glm::vec4 intersect(glm::vec4 a, glm::vec4 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::min(a.z, b.z), std::min(a.w, b.w)};
}

void place(const UiWidget& w, glm::vec4 rect, glm::vec4 clip, const std::string& path, int depth,
           const std::map<std::string, float>& scroll, std::map<std::string, float>* contentHeight, std::vector<UiPlaced>& out) {
    out.push_back({&w, path, rect, clip, depth});
    if (w.children.empty()) return;
    // The content area: inside the padding.
    const glm::vec4 content{rect.x + w.padding.x, rect.y + w.padding.y, std::max(rect.z - w.padding.x - w.padding.z, 0.0f),
                            std::max(rect.w - w.padding.y - w.padding.w, 0.0f)};
    const glm::vec4 inner = w.scroll ? intersect(clip, {content.x, content.y, content.x + content.z, content.y + content.w}) : clip;
    const auto it = scroll.find(path);
    const float offset = w.scroll && it != scroll.end() ? it->second : 0.0f;
    float bottom = content.y;  // lowest child edge (scrolled content height)
    auto child = [&](std::size_t i, glm::vec4 r) {
        r.y -= offset;
        bottom = std::max(bottom, r.y + r.w + offset);
        place(w.children[i], r, inner, path.empty() ? std::to_string(i) : path + "/" + std::to_string(i), depth + 1, scroll,
              contentHeight, out);
    };
    float cursorX = content.x, cursorY = content.y, rowHeight = 0.0f;
    int column = 0;
    const float cellWidth = w.layout == UiWidget::Layout::Grid
                                ? std::max((content.z - w.spacing * static_cast<float>(w.columns - 1)) / static_cast<float>(w.columns), 0.0f)
                                : 0.0f;
    for (std::size_t i = 0; i < w.children.size(); ++i) {
        const UiWidget& c = w.children[i];
        if (!c.visible) continue;
        switch (w.layout) {
            case UiWidget::Layout::None: {
                const glm::vec2 p0 = glm::vec2(content.x, content.y) + c.anchorMin * glm::vec2(content.z, content.w) + c.offsetMin;
                const glm::vec2 p1 = glm::vec2(content.x, content.y) + c.anchorMax * glm::vec2(content.z, content.w) + c.offsetMax;
                child(i, {p0.x, p0.y, std::max(p1.x - p0.x, 0.0f), std::max(p1.y - p0.y, 0.0f)});
                break;
            }
            case UiWidget::Layout::Vertical: {
                const float width = c.size.x > 0.0f ? std::min(c.size.x, content.z) : content.z;
                const float height = c.size.y > 0.0f ? c.size.y : 40.0f;
                child(i, {content.x, cursorY, width, height});
                cursorY += height + w.spacing;
                break;
            }
            case UiWidget::Layout::Horizontal: {
                const float width = c.size.x > 0.0f ? c.size.x : 120.0f;
                const float height = c.size.y > 0.0f ? std::min(c.size.y, content.w) : content.w;
                child(i, {cursorX, content.y, width, height});
                cursorX += width + w.spacing;
                break;
            }
            case UiWidget::Layout::Grid: {
                const float height = c.size.y > 0.0f ? c.size.y : cellWidth;
                child(i, {content.x + static_cast<float>(column) * (cellWidth + w.spacing), cursorY, cellWidth, height});
                rowHeight = std::max(rowHeight, height);
                if (++column >= w.columns) {
                    column = 0;
                    cursorY += rowHeight + w.spacing;
                    rowHeight = 0.0f;
                }
                break;
            }
        }
    }
    if (contentHeight != nullptr && w.scroll) (*contentHeight)[path] = bottom - content.y;
}

UiWidget* find(UiWidget& w, std::string_view name) {
    if (w.name == name) return &w;
    for (UiWidget& c : w.children) {
        if (UiWidget* found = find(c, name)) return found;
    }
    return nullptr;
}

} // namespace

std::string_view uiTypeName(UiWidget::Type type) noexcept {
    for (const auto& [t, name] : kTypes) {
        if (t == type) return name;
    }
    return "panel";
}

nlohmann::json uiToJson(const UiDocument& document) { return {{"version", 1}, {"root", widgetToJson(document.root)}}; }

Result<UiDocument> uiFromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("root")) return makeError("a UI document is a JSON object with a 'root' widget");
    auto root = widgetFromJson(json["root"], 0);
    if (!root) return std::unexpected(root.error());
    if (root->type != UiWidget::Type::Panel) return makeError("the root widget must be a panel");
    return UiDocument{std::move(*root)};
}

Result<UiDocument> loadUiDocument(const std::filesystem::path& file) {
    auto text = fs::readTextFile(file);
    if (!text) return std::unexpected(text.error());
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded()) return makeError("{}: not valid JSON", file.filename().string());
    auto doc = uiFromJson(j);
    if (!doc) return makeError("{}: {}", file.filename().string(), doc.error().message);
    return doc;
}

Result<> saveUiDocument(const UiDocument& document, const std::filesystem::path& file) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    return fs::writeTextFileAtomic(file, uiToJson(document).dump(2) + "\n");
}

UiDocument defaultUiDocument() {
    UiDocument doc;
    doc.root.type = UiWidget::Type::Panel;
    doc.root.name = "Screen";
    doc.root.anchorMin = {0.0f, 0.0f};
    doc.root.anchorMax = {1.0f, 1.0f};
    doc.root.offsetMin = doc.root.offsetMax = glm::vec2(0.0f);
    doc.root.color = glm::vec4(0.0f);  // no fill
    return doc;
}

std::vector<UiPlaced> layoutUi(const UiDocument& document, glm::vec2 screen, const std::map<std::string, float>& scroll,
                               std::map<std::string, float>* contentHeight) {
    std::vector<UiPlaced> out;
    if (!document.root.visible) return out;
    const UiWidget& r = document.root;
    // The root is placed by its anchors on the screen too.
    const glm::vec2 p0 = r.anchorMin * screen + r.offsetMin;
    const glm::vec2 p1 = r.anchorMax * screen + r.offsetMax;
    place(r, {p0.x, p0.y, std::max(p1.x - p0.x, 0.0f), std::max(p1.y - p0.y, 0.0f)}, {-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f}, "", 0, scroll,
          contentHeight, out);
    return out;
}

UiWidget* findUiWidget(UiWidget& root, std::string_view name) { return name.empty() ? nullptr : find(root, name); }

const UiWidget* findUiWidget(const UiWidget& root, std::string_view name) {
    return findUiWidget(const_cast<UiWidget&>(root), name);
}

UiWidget* uiWidgetAt(UiWidget& root, std::string_view path) {
    UiWidget* w = &root;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        std::size_t index = 0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), index);
        if (ec != std::errc{} || index >= w->children.size()) return nullptr;
        w = &w->children[index];
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    }
    return w;
}

} // namespace lumora
