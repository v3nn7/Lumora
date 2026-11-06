#pragma once

#include "common/ModuleExport.hpp"

#include <nlohmann/json.hpp>

#include <QGraphicsView>
#include <QWidget>

#include <map>
#include <vector>

class QLabel;
class QTableWidget;

namespace lumora::visual {
struct NodeDef;
}

namespace lumora::editor {

class GraphNodeItem;
class GraphLinkItem;
class GraphPinItem;
class GraphView;

/// Edits a visual script (.lgraph): nodes from the shared node library (runtime/VisualScript.hpp)
/// placed on a canvas and joined by links. Right-click (or Space, or Add Node) opens a searchable
/// list of nodes; drag from a pin to another pin to link them; unlinked inputs show a field for
/// their value; Delete removes the selection. Variables are edited in the table on the right.
/// Saving writes the file, which a running game reloads at once.
class LUMORA_EDITOR_API GraphEditorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit GraphEditorPanel(QString projectRoot, QWidget* parent = nullptr);
    ~GraphEditorPanel() override;

    bool open(const QString& file);
    bool save();
    [[nodiscard]] const QString& file() const noexcept { return file_; }
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }

    // Editing API (used by the UI and by tests). Node ids are the file's ids.
    int addNode(std::string_view type, QPointF position);
    bool removeNode(int id);
    /// Links an output pin to an input pin; replaces an existing link into a data input.
    bool connectPins(int fromNode, std::string_view fromPin, int toNode, std::string_view toPin);
    bool disconnectInput(int node, std::string_view pin);
    void setLiteral(int node, std::string_view pin, const nlohmann::json& value);
    void addVariable(const QString& name, const QString& type);
    [[nodiscard]] QStringList variableNames() const;
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t linkCount() const noexcept { return links_.size(); }
    /// The graph as saved (JSON document).
    [[nodiscard]] nlohmann::json document() const;
    /// Opens the node list at a scene position (Space / right-click / Add Node).
    void showNodePicker(QPointF scenePosition);
    /// Zooms so the whole graph is visible (at most 100 %).
    void fitGraph();

    /// Whether a link from an output of type `from` into an input of type `to` is allowed.
    [[nodiscard]] static bool compatible(int fromType, int toType);

signals:
    void dirtyChanged(bool dirty);

private:
    friend class GraphView;
    friend class GraphNodeItem;
    friend class GraphPinItem;
    struct Link {
        int fromNode;
        std::string fromPin;
        int toNode;
        std::string toPin;
        GraphLinkItem* item = nullptr;
    };
    void clear();
    void setDirty(bool dirty);
    void rebuildLinkItem(Link& link);
    void refreshLinks();
    void refreshLiteralEditors();
    void removeSelection();
    void loadVariables(const nlohmann::json& variables);
    [[nodiscard]] nlohmann::json variablesJson() const;
    [[nodiscard]] GraphPinItem* pin(int node, std::string_view name, bool output) const;
    void pinDragFinished(GraphPinItem* from, QPointF scenePos);

    QString root_;
    QString file_;
    bool dirty_ = false;
    bool loading_ = false;
    int nextId_ = 1;
    GraphView* view_ = nullptr;
    class QGraphicsScene* scene_ = nullptr;
    QLabel* title_ = nullptr;
    QTableWidget* variables_ = nullptr;
    std::map<int, GraphNodeItem*> nodes_;
    std::vector<Link> links_;
};

/// The canvas: pans with the middle mouse button (or Alt + left), zooms with the wheel.
class GraphView final : public QGraphicsView {
    Q_OBJECT
public:
    GraphView(QGraphicsScene* scene, GraphEditorPanel& panel);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;

private:
    GraphEditorPanel& panel_;
    bool panning_ = false;
    QPoint lastPan_;
};

} // namespace lumora::editor
