#pragma once

#include "common/ModuleExport.hpp"

#include "core/Uuid.hpp"

#include <QTreeWidget>
#include <QWidget>

#include <unordered_set>

class QLineEdit;

namespace lumora::editor {

class EditorContext;

/// Tree widget that turns drag & drop into scene re-parenting requests instead of moving
/// items itself; the panel rebuilds from the scene afterwards (scene is the source of truth).
class LUMORA_EDITOR_API HierarchyTree final : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;

signals:
    void reparentRequested(lumora::Uuid child, lumora::Uuid parent, int index);
    /// Files dragged in from elsewhere (the Content Browser) and dropped on an entity (or on
    /// empty space: a null Uuid).
    void filesDropped(lumora::Uuid target, const QStringList& files);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

class LUMORA_EDITOR_API SceneHierarchyPanel final : public QWidget {
    Q_OBJECT
public:
    explicit SceneHierarchyPanel(EditorContext& ctx, QWidget* parent = nullptr);
    [[nodiscard]] HierarchyTree* tree() const noexcept { return tree_; }

signals:
    void filesDropped(lumora::Uuid target, const QStringList& files);

private:
    void rebuild();
    void syncSelection();
    void applyFilter();
    void showContextMenu(const QPoint& pos);

    EditorContext& ctx_;
    HierarchyTree* tree_ = nullptr;
    QLineEdit* filter_ = nullptr;
    std::unordered_set<Uuid> collapsed_;
    bool rebuilding_ = false;
};

} // namespace lumora::editor
