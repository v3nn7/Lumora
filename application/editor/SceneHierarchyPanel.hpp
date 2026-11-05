#pragma once

#include "core/Uuid.hpp"

#include <QTreeWidget>
#include <QWidget>

#include <unordered_set>

class QLineEdit;

namespace lumora::editor {

class EditorContext;

/// Tree widget that turns drag & drop into scene re-parenting requests instead of moving
/// items itself; the panel rebuilds from the scene afterwards (scene is the source of truth).
class HierarchyTree final : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;

signals:
    void reparentRequested(lumora::Uuid child, lumora::Uuid parent, int index);

protected:
    void dropEvent(QDropEvent* event) override;
};

class SceneHierarchyPanel final : public QWidget {
    Q_OBJECT
public:
    explicit SceneHierarchyPanel(EditorContext& ctx, QWidget* parent = nullptr);

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
