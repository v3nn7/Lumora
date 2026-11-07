#pragma once

#include "core/Error.hpp"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lumora {

/// A widget of a game UI document (.lui): kept between steps (unlike GameUi's immediate
/// widgets), placed by anchors in its parent or by the parent's stack or grid.
struct UiWidget {
    enum class Type { Panel, Label, Button, Checkbox, Slider, Image };
    /// How a panel places its children: by their own anchors, or in a column, row or grid.
    enum class Layout { None, Vertical, Horizontal, Grid };

    Type type = Type::Panel;
    std::string name;  // what scripts ask for ("Play", "Volume")

    // Placement (Layout::None parents): the rectangle spans from anchorMin to anchorMax of the
    // parent's content area (0..1 each way) moved by the offsets in pixels. Equal anchors: a
    // fixed-size box pinned there; 0 and 1: stretched with margins.
    glm::vec2 anchorMin{0.0f};
    glm::vec2 anchorMax{0.0f};
    glm::vec2 offsetMin{0.0f};
    glm::vec2 offsetMax{160.0f, 40.0f};
    /// In stacks and grids: width and height; 0 = the row's / column's full extent.
    glm::vec2 size{0.0f, 40.0f};
    bool visible = true;

    // Look and content.
    glm::vec4 color{1.0f};    // panel fill, text colour, image tint
    std::string text;         // "@key" is looked up in the localisation tables
    float fontSize = 22.0f;
    std::string font;         // project font; empty = built-in
    int align = 0;            // labels: 0 left, 1 centre, 2 right
    std::string image;        // Image widgets: a project texture

    // Values: checkbox 0/1, slider within [min, max].
    float value = 0.0f;
    float min = 0.0f;
    float max = 1.0f;

    // Panels.
    Layout layout = Layout::None;
    int columns = 2;
    float spacing = 8.0f;
    glm::vec4 padding{0.0f};  // left, top, right, bottom
    bool scroll = false;      // children beyond the panel scroll (mouse wheel), cut to it

    std::vector<UiWidget> children;
};

struct UiDocument {
    UiWidget root;  // a panel over the whole screen (no fill by default)
};

inline constexpr const char* kUiExtension = ".lui";

[[nodiscard]] nlohmann::json uiToJson(const UiDocument& document);
[[nodiscard]] Result<UiDocument> uiFromJson(const nlohmann::json& json);
[[nodiscard]] Result<UiDocument> loadUiDocument(const std::filesystem::path& file);
[[nodiscard]] Result<> saveUiDocument(const UiDocument& document, const std::filesystem::path& file);
/// A new document: an empty root panel.
[[nodiscard]] UiDocument defaultUiDocument();

[[nodiscard]] std::string_view uiTypeName(UiWidget::Type type) noexcept;

/// A widget with its place on the screen.
struct UiPlaced {
    const UiWidget* widget = nullptr;
    std::string path;            // "0/2/1": child indices from the root (stable per document)
    glm::vec4 rect{0.0f};        // x, y, width, height
    glm::vec4 clip{-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f};  // x0, y0, x1, y1 it is cut to
    int depth = 0;
};

/// Where every visible widget goes on a screen of this size, parents before children.
/// `scroll` holds scrolled panels' offsets (by path); `contentHeight` receives how tall each
/// scrolled panel's content is (for limiting the scroll).
[[nodiscard]] std::vector<UiPlaced> layoutUi(const UiDocument& document, glm::vec2 screen,
                                             const std::map<std::string, float>& scroll = {},
                                             std::map<std::string, float>* contentHeight = nullptr);

/// The first widget with this name (depth first), or null.
[[nodiscard]] UiWidget* findUiWidget(UiWidget& root, std::string_view name);
[[nodiscard]] const UiWidget* findUiWidget(const UiWidget& root, std::string_view name);
/// The widget at a path ("0/2"; "" = the root), or null.
[[nodiscard]] UiWidget* uiWidgetAt(UiWidget& root, std::string_view path);

} // namespace lumora
