#pragma once

#include "common/ModuleExport.hpp"

#include "editor/PropertyWidgets.hpp"
#include "runtime/UiDocument.hpp"

#include <nlohmann/json.hpp>

#include <QImage>
#include <QWidget>

#include <map>
#include <vector>

class QComboBox;
class QFormLayout;
class QLabel;
class QTreeWidget;

namespace lumora::editor {

class UiCanvas;

/// Edits a game UI document (.lui): the widget tree on the left, the screen in the middle
/// (laid out as the game lays it out, at a chosen resolution; click to select, drag to move,
/// drag the corner handle to resize), the selected widget's properties on the right.
/// Anchor presets keep the widget where it is. Ctrl+Z / Ctrl+Shift+Z undo and redo; Ctrl+S saves.
class LUMORA_EDITOR_API UiEditorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit UiEditorPanel(QString projectRoot, AssetPicker pickAsset = {}, QWidget* parent = nullptr);
    ~UiEditorPanel() override;

    bool open(const QString& file);
    bool save();
    [[nodiscard]] const QString& file() const noexcept { return file_; }
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
    [[nodiscard]] const UiDocument& document() const noexcept { return document_; }

    // Editing API (the UI and tests). Paths as in UiPlaced ("" = root, "0/2" = child 2 of child 0).
    /// Adds a widget into the panel at `parent` (or the selected widget's panel); its path.
    QString addWidget(UiWidget::Type type, const QString& parent = QString());
    bool removeWidget(const QString& path);
    /// Moves a child one place up (-1) or down (+1) among its siblings; its new path.
    QString moveInParent(const QString& path, int delta);
    void select(const QString& path);
    [[nodiscard]] const QString& selected() const noexcept { return selected_; }
    /// Drags a widget by screen pixels (its offsets; in stacks only the size can change).
    void moveWidget(const QString& path, glm::vec2 delta);
    void resizeWidget(const QString& path, glm::vec2 delta);
    /// Anchors the widget (min, max of the parent's content area) without moving it.
    void setAnchors(const QString& path, glm::vec2 anchorMin, glm::vec2 anchorMax);
    /// Changes the selected widget through `edit` (one undo step).
    void editSelected(const std::function<void(UiWidget&)>& edit);
    void undo();
    void redo();
    void setResolution(glm::vec2 size);
    [[nodiscard]] glm::vec2 resolution() const noexcept { return resolution_; }
    /// Where each widget is, at the current resolution.
    [[nodiscard]] std::vector<UiPlaced> layout() const;
    /// The canvas as drawn (tests, previews).
    [[nodiscard]] QImage canvasImage() const;

signals:
    void dirtyChanged(bool dirty);
    /// A document was written (a running game may reopen it).
    void saved(const QString& file);

private:
    friend class UiCanvas;
    // commit = false: a drag in progress (one undo step when it ends).
    void moveWidget(const QString& path, glm::vec2 delta, bool commit);
    void resizeWidget(const QString& path, glm::vec2 delta, bool commit);
    [[nodiscard]] UiWidget* widgetAt(const QString& path);
    void changed();  // the document changed: undo step, dirty, views
    void setDirty(bool dirty);
    void rebuildTree();
    void rebuildProperties();
    void refreshViews();
    [[nodiscard]] QString panelFor(const QString& path) const;  // the panel a new widget goes into
    [[nodiscard]] QImage image(const std::string& key);           // project textures, cached

    QString root_;
    QString file_;
    UiDocument document_;
    QString selected_;
    glm::vec2 resolution_{1280.0f, 720.0f};
    bool dirty_ = false;
    std::vector<nlohmann::json> undo_, redo_;
    nlohmann::json committed_;
    AssetPicker pickAsset_;
    QTreeWidget* tree_ = nullptr;
    UiCanvas* canvas_ = nullptr;
    QWidget* properties_ = nullptr;
    QFormLayout* form_ = nullptr;
    QLabel* title_ = nullptr;
    QComboBox* resolutionBox_ = nullptr;
    std::vector<Refresher> refreshers_;
    std::vector<std::unique_ptr<FieldDesc>> fieldDescs_;  // the property editors keep references
    std::map<std::string, QImage> images_;
    bool rebuilding_ = false;
};

} // namespace lumora::editor
