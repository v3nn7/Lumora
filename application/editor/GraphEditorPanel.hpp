#pragma once

#include "common/ModuleExport.hpp"

#include <nlohmann/json.hpp>

#include <QGraphicsView>
#include <QImage>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <vector>

class QLabel;
class QMenu;
class QTableWidget;
class QTimer;

namespace lumora::visual {
struct NodeDef;
}

namespace lumora::editor {

class GraphNodeItem;
class GraphLinkItem;
class GraphPinItem;
class GraphView;
class GraphCommentItem;

/// Edits a visual script (.lgraph): nodes from the shared node library (runtime/VisualScript.hpp)
/// placed on a canvas and joined by links. Right-click (or Space, or Add Node) opens a searchable
/// list of nodes, and on a node, pin or link a menu to delete it or cut its links; drag from a
/// pin to another pin to link them, or to empty canvas for a list of nodes that fit; unlinked
/// inputs show a field for their value; Delete removes the selection. Ctrl+Z / Ctrl+Shift+Z undo
/// and redo, Ctrl+C / X / V / D copy, cut, paste and duplicate, C frames the selection in a
/// comment, double-clicking a link adds a reroute dot. Variables are edited in the table on the
/// right; drag one onto the canvas for a Get or Set node. While the game plays, the nodes and
/// links that run light up and output pins show their values.
/// Saving writes the file, which a running game reloads at once.
/// What a graph panel edits: visual scripts (.lgraph) or material graphs (.lmgraph), each with
/// its own node library. Material graphs have no variables and no live view; they are saved
/// a moment after every change (the scene shows it at once), compiled to report problems, and
/// show a sphere wearing the material.
enum class GraphKind { VisualScript, Material };

class LUMORA_EDITOR_API GraphEditorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit GraphEditorPanel(QString projectRoot, GraphKind kind = GraphKind::VisualScript, QWidget* parent = nullptr);
    ~GraphEditorPanel() override;

    [[nodiscard]] GraphKind kind() const noexcept { return kind_; }
    /// Material graphs: renders the preview sphere (absolute .lmgraph path, size in pixels).
    void setMaterialPreview(std::function<QImage(const QString&, int)> preview);
    /// Material graphs: why the graph does not compile (empty when it does).
    [[nodiscard]] const QString& compileError() const noexcept { return compileError_; }
    /// Material graphs: the preview image (null without a preview renderer).
    [[nodiscard]] QImage previewImage() const;

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
    /// Removes every link on one pin (an output can feed many inputs); returns how many.
    int disconnectPin(int node, std::string_view pin, bool output);
    /// Removes every link into or out of a node; returns how many.
    int disconnectNode(int node);
    /// Removes the link from an output to an input.
    bool removeLink(int fromNode, std::string_view fromPin, int toNode, std::string_view toPin);
    /// A copy of a node with its field values (not its links), placed next to it; returns its id.
    int duplicateNode(int id);
    void setLiteral(int node, std::string_view pin, const nlohmann::json& value);
    void addVariable(const QString& name, const QString& type);
    [[nodiscard]] QStringList variableNames() const;
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t linkCount() const noexcept { return links_.size(); }
    /// The graph as saved (JSON document).
    [[nodiscard]] nlohmann::json document() const;
    // Undo history: every change is recorded (edits made in quick succession, such as a drag,
    // as one step) and can be undone and redone.
    void undo();
    void redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;

    // Clipboard: nodes, the links between them and comments, as JSON (also valid text).
    [[nodiscard]] std::vector<int> selectedNodes() const;
    void selectNodes(const std::vector<int>& ids);
    void selectAll();
    void copySelection();
    void cutSelection();
    /// Pastes at a scene position (top-left of what was copied), or next to the original.
    bool paste(std::optional<QPointF> at = std::nullopt);
    void duplicateSelection();

    /// A titled frame behind nodes; moving it moves the nodes inside. Returns its index.
    int addComment(const QRectF& rect, const QString& text);
    /// Frames the selected nodes in a new comment (or adds one in the middle of the view).
    void commentSelection();
    [[nodiscard]] std::size_t commentCount() const noexcept { return comments_.size(); }

    /// Splits a link with a reroute dot at a position; returns the dot's node id.
    int insertReroute(int fromNode, std::string_view fromPin, int toNode, std::string_view toPin, QPointF at);
    /// A Get Variable (or Set Variable) node for a variable, at a scene position.
    int addVariableNode(const QString& name, bool set, QPointF at);

    /// Live view while the game plays: reads the runtime's trace of this graph (also on a timer).
    void refreshLive();
    [[nodiscard]] bool isLive() const noexcept { return live_; }
    /// How brightly a node is lit (1 = ran this step, fading to 0).
    [[nodiscard]] float nodeGlow(int id) const;
    /// The last value on an output pin while live (empty when unknown).
    [[nodiscard]] QString liveValue(int node, std::string_view pin) const;

    /// Opens the node list at a scene position (Space / right-click / Add Node).
    void showNodePicker(QPointF scenePosition);
    /// The right-click menu for what is at a scene position: a node (delete, duplicate, break
    /// its links), a pin (break its links) or a link (remove it). Null on empty canvas, where
    /// right-click opens the node list instead.
    [[nodiscard]] QMenu* contextMenuAt(QPointF scenePosition, QWidget* parent);
    /// Zooms so the whole graph is visible (at most 100 %).
    void fitGraph();

    /// Whether a link from an output of type `from` into an input of type `to` is allowed.
    [[nodiscard]] static bool compatible(int fromType, int toType);

signals:
    void dirtyChanged(bool dirty);
    /// A material graph was written (the scene should re-read it).
    void materialSaved(const QString& file);

private:
    friend class GraphView;
    friend class GraphNodeItem;
    friend class GraphPinItem;
    friend class GraphCommentItem;
    struct Link {
        int fromNode;
        std::string fromPin;
        int toNode;
        std::string toPin;
        GraphLinkItem* item = nullptr;
    };
    void clear();
    void setDirty(bool dirty);
    /// Something changed: marks the graph dirty and records an undo step soon.
    void changed();
    void commitUndo();
    void restore(const nlohmann::json& document);
    /// Replaces the graph with a document (nodes, links, variables, comments).
    void loadDocument(const nlohmann::json& document);
    [[nodiscard]] nlohmann::json selectionDocument() const;
    /// Adds a copied document's nodes, links and comments, moved to `at` (or `step` x 40 px on).
    bool pasteDocument(const nlohmann::json& doc, std::optional<QPointF> at, int step);
    void removeComment(GraphCommentItem* comment);
    void dropVariable(const QString& name, QPointF scenePos, Qt::KeyboardModifiers modifiers);
    void rebuildLinkItem(Link& link);
    void refreshLinks();
    void refreshLiteralEditors();
    void removeSelection();
    void loadVariables(const nlohmann::json& variables);
    [[nodiscard]] nlohmann::json variablesJson() const;
    [[nodiscard]] GraphPinItem* pin(int node, std::string_view name, bool output) const;
    void pinDragFinished(GraphPinItem* from, QPointF scenePos);
    [[nodiscard]] const visual::NodeDef* findNode(std::string_view type) const;
    [[nodiscard]] const std::vector<visual::NodeDef>& library() const;
    /// Whether a link between pins of these types may be made in this kind of graph.
    [[nodiscard]] bool canLink(int fromType, int toType) const;
    void materialChanged();

    QString root_;
    QString file_;
    bool dirty_ = false;
    bool loading_ = false;
    int nextId_ = 1;
    GraphView* view_ = nullptr;
    class QGraphicsScene* scene_ = nullptr;
    QLabel* title_ = nullptr;
    QTableWidget* variables_ = nullptr;
    QLabel* liveLabel_ = nullptr;
    std::map<int, GraphNodeItem*> nodes_;
    std::vector<Link> links_;
    std::vector<GraphCommentItem*> comments_;
    std::vector<nlohmann::json> undo_;
    std::vector<nlohmann::json> redo_;
    nlohmann::json committed_;  // the state the last undo step ends in
    nlohmann::json saved_;      // the state on disk (dirty = differs)
    QTimer* commitTimer_ = nullptr;
    QTimer* liveTimer_ = nullptr;
    std::string liveKey_;
    bool live_ = false;
    bool restoring_ = false;
    int pasteCount_ = 0;
    GraphKind kind_ = GraphKind::VisualScript;
    QLabel* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* autoSave_ = nullptr;
    QString compileError_;
    std::function<QImage(const QString&, int)> previewRenderer_;
};

/// The canvas: pans with the middle mouse button (or Alt + left), zooms with the wheel.
class GraphView final : public QGraphicsView {
    Q_OBJECT
public:
    GraphView(QGraphicsScene* scene, GraphEditorPanel& panel);

protected:
    bool event(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
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
