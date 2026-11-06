#include "editor/GraphEditorPanel.hpp"

#include "common/Theme.hpp"
#include "runtime/ScriptNames.hpp"
#include "runtime/VisualScript.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>

namespace lumora::editor {

using visual::NodeDef;
using visual::PinDef;
using visual::PinType;

namespace {

constexpr qreal kNodeWidth = 300.0;
constexpr qreal kHeader = 26.0;
constexpr qreal kRow = 26.0;
constexpr qreal kPinRadius = 6.0;

QColor pinColor(PinType type) {
    switch (type) {
    case PinType::Exec: return QColor(0xf0, 0xf0, 0xf0);
    case PinType::Number: return QColor(0x7e, 0xd3, 0x21);
    case PinType::Bool: return QColor(0xe0, 0x4f, 0x4f);
    case PinType::Vec3: return QColor(0xf5, 0xc2, 0x42);
    case PinType::String: return QColor(0xe0, 0x6c, 0xd6);
    case PinType::Entity: return QColor(0x4f, 0xb4, 0xff);
    case PinType::Any: return QColor(0xa0, 0xa6, 0xb0);
    }
    return Qt::gray;
}

QColor categoryColor(std::string_view category) {
    if (category == "Events") return QColor(0xb0, 0x3a, 0x3a);
    if (category == "Flow") return QColor(0x5a, 0x5f, 0x6a);
    if (category == "Variables") return QColor(0x7a, 0x4a, 0xa8);
    if (category == "Math" || category == "Logic" || category == "Vector" || category == "Text") return QColor(0x3a, 0x7a, 0x4a);
    if (category == "Entity" || category == "Physics" || category == "Character") return QColor(0x2f, 0x5f, 0xa8);
    if (category == "Input") return QColor(0xa8, 0x6a, 0x2a);
    if (category == "HUD" || category == "Debug") return QColor(0x8a, 0x3a, 0x7a);
    return QColor(0x3a, 0x6a, 0x7a);
}

QString label(const PinDef& pin) {
    const std::string_view text = pin.label.empty() ? pin.name : pin.label;
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

nlohmann::json defaultLiteral(const PinDef& pin) {
    switch (pin.type) {
    case PinType::Number: return pin.defaultNumber;
    case PinType::Bool: return pin.defaultNumber != 0.0;
    case PinType::Vec3: return visual::Value::ofVec3(visual::Value::ofString(std::string(pin.defaultText)).asVec3()).toJson();
    case PinType::String: return std::string(pin.defaultText);
    case PinType::Entity: return std::string();
    default: return 0.0;
    }
}

template <std::size_t N>
QStringList names(const std::array<std::string_view, N>& list) {
    QStringList out;
    for (std::string_view n : list) out << qs(n);
    return out;
}

} // namespace

// ---- Pins, nodes, links ---------------------------------------------------------------------

class GraphPinItem final : public QGraphicsItem {
public:
    GraphPinItem(GraphEditorPanel& panel, int node, const PinDef& def, bool output, QGraphicsItem* parent)
        : QGraphicsItem(parent), panel_(panel), node_(node), def_(def), output_(output) {
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::CrossCursor);
        setToolTip(QStringLiteral("%1 (%2)").arg(label(def), qs(visual::pinTypeName(def.type))));
    }
    [[nodiscard]] QRectF boundingRect() const override {
        return {-kPinRadius - 2, -kPinRadius - 2, 2 * kPinRadius + 4, 2 * kPinRadius + 4};
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        const QColor c = pinColor(def_.type);
        p->setPen(QPen(c, 1.5));
        p->setBrush(linked ? c : QColor(0x20, 0x22, 0x27));
        if (def_.type == PinType::Exec) {
            QPolygonF tri;
            tri << QPointF(-kPinRadius + 1, -kPinRadius) << QPointF(kPinRadius, 0) << QPointF(-kPinRadius + 1, kPinRadius);
            p->drawPolygon(tri);
        } else {
            p->drawEllipse(QPointF(0, 0), kPinRadius - 1, kPinRadius - 1);
        }
    }
    [[nodiscard]] int node() const { return node_; }
    [[nodiscard]] const PinDef& def() const { return def_; }
    [[nodiscard]] bool output() const { return output_; }
    bool linked = false;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        drag_ = new QGraphicsPathItem;
        drag_->setPen(QPen(pinColor(def_.type), 2, Qt::DashLine));
        drag_->setZValue(10);
        scene()->addItem(drag_);
        updateDrag(event->scenePos());
        event->accept();
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override { updateDrag(event->scenePos()); }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        delete drag_;
        drag_ = nullptr;
        panel_.pinDragFinished(this, event->scenePos());
    }

private:
    void updateDrag(QPointF to) {
        const QPointF from = scenePos();
        QPainterPath path(from);
        const qreal dx = std::max(40.0, std::abs(to.x() - from.x()) * 0.5) * (output_ ? 1.0 : -1.0);
        path.cubicTo(from + QPointF(dx, 0), to - QPointF(dx, 0), to);
        drag_->setPath(path);
    }
    GraphEditorPanel& panel_;
    int node_;
    const PinDef& def_;
    bool output_;
    QGraphicsPathItem* drag_ = nullptr;
};

class GraphLinkItem final : public QGraphicsPathItem {
public:
    explicit GraphLinkItem(PinType type) {
        setFlag(QGraphicsItem::ItemIsSelectable);
        setZValue(-1);
        color_ = pinColor(type);
        setPen(QPen(color_, type == PinType::Exec ? 3 : 2));
    }
    void setEnds(QPointF from, QPointF to) {
        QPainterPath path(from);
        const qreal dx = std::max(40.0, std::abs(to.x() - from.x()) * 0.5);
        path.cubicTo(from + QPointF(dx, 0), to - QPointF(dx, 0), to);
        setPath(path);
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        QPen pen = this->pen();
        if (isSelected()) pen.setColor(app::theme::kAccent);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(path());
    }
    [[nodiscard]] QPainterPath shape() const override {
        QPainterPathStroker stroker;
        stroker.setWidth(10);
        return stroker.createStroke(path());
    }

private:
    QColor color_;
};

class GraphNodeItem final : public QGraphicsItem {
public:
    GraphNodeItem(GraphEditorPanel& panel, int id, const NodeDef& def, nlohmann::json literals)
        : panel_(panel), id_(id), def_(def), literals_(std::move(literals)) {
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        setToolTip(qs(def.description));
        if (!literals_.is_object()) literals_ = nlohmann::json::object();
        const int rows = static_cast<int>(std::max(def.inputs.size(), def.outputs.size()));
        height_ = kHeader + std::max(rows, 1) * kRow + 6;
        for (std::size_t i = 0; i < def.inputs.size(); ++i) {
            auto* pin = new GraphPinItem(panel, id, def.inputs[i], false, this);
            pin->setPos(0, kHeader + static_cast<qreal>(i) * kRow + kRow / 2);
            inputs_.push_back(pin);
            editors_.push_back(createEditor(def.inputs[i], static_cast<int>(i)));
        }
        for (std::size_t i = 0; i < def.outputs.size(); ++i) {
            auto* pin = new GraphPinItem(panel, id, def.outputs[i], true, this);
            pin->setPos(kNodeWidth, kHeader + static_cast<qreal>(i) * kRow + kRow / 2);
            outputs_.push_back(pin);
        }
    }

    [[nodiscard]] QRectF boundingRect() const override { return {-2, -2, kNodeWidth + 4, height_ + 4}; }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF body(0, 0, kNodeWidth, height_);
        p->setPen(QPen(isSelected() ? app::theme::kAccent : app::theme::kBorder, isSelected() ? 2 : 1));
        p->setBrush(QColor(0x22, 0x25, 0x2b, 235));
        p->drawRoundedRect(body, 6, 6);
        QPainterPath header;
        header.addRoundedRect(QRectF(0, 0, kNodeWidth, kHeader), 6, 6);
        p->fillPath(header, categoryColor(def_.category));
        p->setPen(Qt::white);
        QFont f = p->font();
        f.setBold(true);
        p->setFont(f);
        p->drawText(QRectF(10, 0, kNodeWidth - 20, kHeader), Qt::AlignVCenter | Qt::AlignLeft, qs(def_.title));
        f.setBold(false);
        p->setFont(f);
        p->setPen(app::theme::kText);
        for (std::size_t i = 0; i < def_.inputs.size(); ++i) {
            if (def_.inputs[i].type == PinType::Exec) continue;
            p->drawText(QRectF(12, kHeader + static_cast<qreal>(i) * kRow, 82, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                        label(def_.inputs[i]));
        }
        for (std::size_t i = 0; i < def_.outputs.size(); ++i) {
            if (def_.outputs[i].type == PinType::Exec && def_.outputs[i].label == " ") continue;
            p->drawText(QRectF(kNodeWidth - 92, kHeader + static_cast<qreal>(i) * kRow, 80, kRow),
                        Qt::AlignVCenter | Qt::AlignRight, label(def_.outputs[i]));
        }
    }

    [[nodiscard]] int id() const { return id_; }
    [[nodiscard]] const NodeDef& def() const { return def_; }
    [[nodiscard]] nlohmann::json& literals() { return literals_; }
    [[nodiscard]] GraphPinItem* pin(std::string_view name, bool output) const {
        for (GraphPinItem* p : output ? outputs_ : inputs_) {
            if (p->def().name == name) return p;
        }
        return nullptr;
    }
    [[nodiscard]] const std::vector<GraphPinItem*>& inputs() const { return inputs_; }
    [[nodiscard]] const std::vector<GraphPinItem*>& outputs() const { return outputs_; }
    /// Literal fields are shown only for inputs without a link.
    void refreshEditors() {
        for (std::size_t i = 0; i < editors_.size(); ++i) {
            if (editors_[i] != nullptr) editors_[i]->setVisible(!inputs_[i]->linked);
        }
    }
    void reloadVariables(const QStringList& variables) {
        for (std::size_t i = 0; i < editors_.size(); ++i) {
            if (editors_[i] == nullptr || def_.inputs[i].hint != visual::PinHint::Variable) continue;
            auto* combo = qobject_cast<QComboBox*>(editors_[i]->widget());
            if (combo == nullptr) continue;
            const QString current = combo->currentText();
            const QSignalBlocker block(combo);
            combo->clear();
            combo->addItems(variables);
            combo->setCurrentText(current);
        }
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        if (change == ItemPositionHasChanged) {
            panel_.refreshLinks();
            if (!panel_.loading_) panel_.setDirty(true);
        }
        return QGraphicsItem::itemChange(change, value);
    }

private:
    QGraphicsProxyWidget* createEditor(const PinDef& pin, int index) {
        if (pin.type == PinType::Exec) return nullptr;
        const std::string key(pin.name);
        if (!literals_.contains(key)) literals_[key] = defaultLiteral(pin);
        const nlohmann::json value = literals_[key];
        QWidget* widget = nullptr;
        auto commit = [this, key](nlohmann::json v) { panel_.setLiteral(id_, key, std::move(v)); };
        auto combo = [&](const QStringList& items, bool editable) {
            auto* c = new QComboBox;
            c->setEditable(editable);
            c->addItems(items);
            c->setCurrentText(value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString());
            QObject::connect(c, &QComboBox::currentTextChanged, c, [commit](const QString& t) { commit(t.toStdString()); });
            return c;
        };
        switch (pin.type) {
        case PinType::Number: {
            auto* spin = new QDoubleSpinBox;
            spin->setRange(-1e7, 1e7);
            spin->setDecimals(3);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setValue(value.is_number() ? value.get<double>() : 0.0);
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [commit](double v) { commit(v); });
            widget = spin;
            break;
        }
        case PinType::Bool: {
            auto* check = new QCheckBox;
            check->setChecked(value.is_boolean() && value.get<bool>());
            QObject::connect(check, &QCheckBox::toggled, check, [commit](bool v) { commit(v); });
            widget = check;
            break;
        }
        case PinType::Vec3: {
            auto* edit = new QLineEdit;
            const auto v = visual::Value::fromJson(value, PinType::Vec3).vec;
            edit->setText(QStringLiteral("%1, %2, %3").arg(v.x).arg(v.y).arg(v.z));
            edit->setPlaceholderText(QStringLiteral("x, y, z"));
            QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, commit] {
                commit(visual::Value::ofVec3(visual::Value::ofString(edit->text().toStdString()).asVec3()).toJson());
            });
            widget = edit;
            break;
        }
        default:
            switch (pin.hint) {
            case visual::PinHint::Key: widget = combo(names(script_names::kKeys), false); break;
            case visual::PinHint::MouseButton: widget = combo(names(script_names::kMouseButtons), false); break;
            case visual::PinHint::GamepadButton: widget = combo(names(script_names::kGamepadButtons), false); break;
            case visual::PinHint::GamepadAxis: widget = combo(names(script_names::kGamepadAxes), false); break;
            case visual::PinHint::Variable: widget = combo(panel_.variableNames(), true); break;
            default: {
                auto* edit = new QLineEdit;
                edit->setText(value.is_string() ? QString::fromStdString(value.get<std::string>())
                              : value.is_null() ? QString()
                                                : QString::fromStdString(value.dump()));
                if (pin.type == PinType::Entity) edit->setPlaceholderText(QObject::tr("self (or a name)"));
                const bool any = pin.type == PinType::Any;
                QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, commit, any] {
                    const QString text = edit->text();
                    bool number = false;
                    const double n = text.toDouble(&number);
                    if (any && number) {
                        commit(n);
                    } else {
                        commit(text.toStdString());
                    }
                });
                widget = edit;
                break;
            }
            }
        }
        widget->setFixedSize(108, static_cast<int>(kRow) - 4);
        widget->setStyleSheet(QStringLiteral("font-size: 11px;"));
        auto* proxy = new QGraphicsProxyWidget(this);
        proxy->setWidget(widget);
        proxy->setPos(94, kHeader + static_cast<qreal>(index) * kRow + 2);
        return proxy;
    }

    GraphEditorPanel& panel_;
    int id_;
    const NodeDef& def_;
    nlohmann::json literals_;
    qreal height_ = 0.0;
    std::vector<GraphPinItem*> inputs_;
    std::vector<GraphPinItem*> outputs_;
    std::vector<QGraphicsProxyWidget*> editors_;  // per input (null for exec pins)
};

// ---- Node picker ------------------------------------------------------------------------------

namespace {

/// Search box over the node library; returns the chosen node type (empty if cancelled).
std::string pickNode(QWidget* parent, QPoint globalPos) {
    QDialog dialog(parent, Qt::Popup);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* search = new QLineEdit;
    search->setPlaceholderText(QObject::tr("Search nodes…"));
    auto* list = new QListWidget;
    layout->addWidget(search);
    layout->addWidget(list);
    dialog.resize(340, 420);
    dialog.move(globalPos);
    auto fill = [&](const QString& filter) {
        list->clear();
        for (const NodeDef& d : visual::nodeLibrary()) {
            const QString title = qs(d.title);
            const QString category = qs(d.category);
            if (!filter.isEmpty() && !title.contains(filter, Qt::CaseInsensitive) && !category.contains(filter, Qt::CaseInsensitive) &&
                !qs(d.type).contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            auto* item = new QListWidgetItem(QStringLiteral("%1   ·  %2").arg(title, category));
            item->setData(Qt::UserRole, qs(d.type));
            item->setToolTip(qs(d.description));
            list->addItem(item);
        }
        if (list->count() > 0) list->setCurrentRow(0);
    };
    fill({});
    std::string chosen;
    auto accept = [&] {
        if (QListWidgetItem* item = list->currentItem()) {
            chosen = item->data(Qt::UserRole).toString().toStdString();
            dialog.accept();
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, fill);
    QObject::connect(search, &QLineEdit::returnPressed, &dialog, accept);
    QObject::connect(list, &QListWidget::itemActivated, &dialog, accept);
    // Up/Down in the search box move through the list.
    struct Keys final : QObject {
        QListWidget* list;
        explicit Keys(QListWidget* l) : list(l) {}
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() != QEvent::KeyPress) return false;
            const int key = static_cast<QKeyEvent*>(e)->key();
            if (key == Qt::Key_Down || key == Qt::Key_Up) {
                const int row = std::clamp(list->currentRow() + (key == Qt::Key_Down ? 1 : -1), 0, list->count() - 1);
                list->setCurrentRow(row);
                return true;
            }
            return false;
        }
    } keys(list);
    search->installEventFilter(&keys);
    search->setFocus();
    dialog.exec();
    return chosen;
}

} // namespace

// ---- GraphView --------------------------------------------------------------------------------

GraphView::GraphView(QGraphicsScene* scene, GraphEditorPanel& panel) : QGraphicsView(scene), panel_(panel) {
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setFocusPolicy(Qt::StrongFocus);
}

void GraphView::wheelEvent(QWheelEvent* event) {
    const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    const qreal current = transform().m11();
    if ((current > 2.5 && factor > 1.0) || (current < 0.2 && factor < 1.0)) return;
    scale(factor, factor);
}

void GraphView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier))) {
        panning_ = true;
        lastPan_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void GraphView::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPoint delta = event->pos() - lastPan_;
        lastPan_ = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void GraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_) {
        panning_ = false;
        unsetCursor();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void GraphView::keyPressEvent(QKeyEvent* event) {
    // Keys typed into a node's field stay there.
    if (scene()->focusItem() != nullptr && dynamic_cast<QGraphicsProxyWidget*>(scene()->focusItem()) != nullptr) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        panel_.removeSelection();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        panel_.showNodePicker(mapToScene(mapFromGlobal(QCursor::pos())));
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void GraphView::contextMenuEvent(QContextMenuEvent* event) {
    // A node's own field keeps its text menu.
    if (dynamic_cast<QGraphicsProxyWidget*>(itemAt(event->pos())) != nullptr) {
        QGraphicsView::contextMenuEvent(event);
        return;
    }
    const QPointF at = mapToScene(event->pos());
    if (std::unique_ptr<QMenu> menu{panel_.contextMenuAt(at, this)}) {
        menu->exec(event->globalPos());
        return;
    }
    panel_.showNodePicker(at);
}

void GraphView::drawBackground(QPainter* painter, const QRectF& rect) {
    painter->fillRect(rect, app::theme::kBackground);
    const qreal step = 32.0;
    QPen minor(QColor(0x1e, 0x20, 0x25));
    QPen major(QColor(0x26, 0x29, 0x30));
    const qreal left = std::floor(rect.left() / step) * step;
    const qreal top = std::floor(rect.top() / step) * step;
    for (qreal x = left; x < rect.right(); x += step) {
        painter->setPen(std::fmod(std::abs(x), step * 4) < 0.5 ? major : minor);
        painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    }
    for (qreal y = top; y < rect.bottom(); y += step) {
        painter->setPen(std::fmod(std::abs(y), step * 4) < 0.5 ? major : minor);
        painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
}

// ---- GraphEditorPanel ---------------------------------------------------------------------------

GraphEditorPanel::GraphEditorPanel(QString projectRoot, QWidget* parent) : QWidget(parent), root_(std::move(projectRoot)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    title_ = new QLabel(tr("No visual script open — create one in the Content Browser (New > Visual Script)"));
    auto* add = new QToolButton;
    add->setIcon(app::theme::icon(QStringLiteral("plus")));
    add->setText(tr("Add Node"));
    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    add->setToolTip(tr("Add a node (Space or right-click on the canvas)"));
    auto* fit = new QToolButton;
    fit->setIcon(app::theme::icon(QStringLiteral("search")));
    fit->setToolTip(tr("Show the whole graph"));
    auto* saveButton = new QToolButton;
    saveButton->setIcon(app::theme::icon(QStringLiteral("save")));
    saveButton->setText(tr("Save"));
    saveButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    saveButton->setToolTip(tr("Save (Ctrl+S); a running game reloads it"));
    bar->addWidget(title_, 1);
    bar->addWidget(add);
    bar->addWidget(fit);
    bar->addWidget(saveButton);
    layout->addLayout(bar);

    scene_ = new QGraphicsScene(this);
    scene_->setSceneRect(-5000, -5000, 10000, 10000);
    view_ = new GraphView(scene_, *this);

    auto* side = new QWidget;
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->addWidget(new QLabel(tr("Variables")));
    variables_ = new QTableWidget(0, 3);
    variables_->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Value")});
    variables_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    variables_->verticalHeader()->hide();
    sideLayout->addWidget(variables_, 1);
    auto* varButtons = new QHBoxLayout;
    auto* addVar = new QPushButton(tr("Add"));
    auto* removeVar = new QPushButton(tr("Remove"));
    varButtons->addWidget(addVar);
    varButtons->addWidget(removeVar);
    sideLayout->addLayout(varButtons);
    auto* help = new QLabel(tr("Drag from a pin to another pin to link them. Space / right-click: add node. "
                               "Right-click a node, pin or link to delete it or cut links. Delete: remove the selection. "
                               "Middle mouse: pan, wheel: zoom."));
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: %1;").arg(app::theme::kTextMuted.name()));
    sideLayout->addWidget(help);

    auto* split = new QSplitter;
    split->addWidget(view_);
    split->addWidget(side);
    split->setStretchFactor(0, 1);
    split->setSizes({700, 220});
    layout->addWidget(split, 1);

    connect(add, &QToolButton::clicked, this, [this] { showNodePicker(view_->mapToScene(view_->viewport()->rect().center())); });
    connect(fit, &QToolButton::clicked, this, &GraphEditorPanel::fitGraph);
    connect(saveButton, &QToolButton::clicked, this, &GraphEditorPanel::save);
    auto* shortcut = new QShortcut(QKeySequence::Save, this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &GraphEditorPanel::save);
    connect(addVar, &QPushButton::clicked, this, [this] {
        QString name = QStringLiteral("value");
        for (int i = 2; variableNames().contains(name); ++i) name = QStringLiteral("value%1").arg(i);
        addVariable(name, QStringLiteral("number"));
    });
    connect(removeVar, &QPushButton::clicked, this, [this] {
        const int row = variables_->currentRow();
        if (row < 0) return;
        variables_->removeRow(row);
        setDirty(true);
        for (auto& [id, node] : nodes_) node->reloadVariables(variableNames());
    });
    connect(variables_, &QTableWidget::cellChanged, this, [this] {
        if (loading_) return;
        setDirty(true);
        for (auto& [id, node] : nodes_) node->reloadVariables(variableNames());
    });
    setEnabled(true);
}

GraphEditorPanel::~GraphEditorPanel() = default;

void GraphEditorPanel::fitGraph() {
    if (nodes_.empty()) return;
    const QRectF bounds = scene_->itemsBoundingRect().adjusted(-40, -40, 40, 40);
    view_->fitInView(bounds, Qt::KeepAspectRatio);
    if (view_->transform().m11() > 1.0) {  // small graphs stay at 100 %
        view_->resetTransform();
        view_->centerOn(bounds.center());
    }
}

void GraphEditorPanel::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    if (!file_.isEmpty()) title_->setText(QFileInfo(file_).fileName() + (dirty ? QStringLiteral(" •") : QString()));
    emit dirtyChanged(dirty);
}

void GraphEditorPanel::clear() {
    links_.clear();
    nodes_.clear();
    scene_->clear();
    variables_->setRowCount(0);
    nextId_ = 1;
}

bool GraphEditorPanel::compatible(int fromType, int toType) {
    const auto from = static_cast<PinType>(fromType);
    const auto to = static_cast<PinType>(toType);
    if ((from == PinType::Exec) != (to == PinType::Exec)) return false;
    if (from == to || from == PinType::Any || to == PinType::Any) return true;
    if (to == PinType::String) return true;  // everything prints
    const auto numeric = [](PinType t) { return t == PinType::Number || t == PinType::Bool; };
    if (numeric(from) && numeric(to)) return true;
    if (numeric(from) && to == PinType::Vec3) return true;
    return false;
}

int GraphEditorPanel::addNode(std::string_view type, QPointF position) {
    const NodeDef* def = visual::findNode(type);
    if (def == nullptr) return 0;
    const int id = nextId_++;
    auto* item = new GraphNodeItem(*this, id, *def, nlohmann::json::object());
    scene_->addItem(item);
    item->setPos(position);
    nodes_[id] = item;
    setDirty(true);
    return id;
}

bool GraphEditorPanel::removeNode(int id) {
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    for (auto l = links_.begin(); l != links_.end();) {
        if (l->fromNode == id || l->toNode == id) {
            delete l->item;
            l = links_.erase(l);
        } else {
            ++l;
        }
    }
    delete it->second;
    nodes_.erase(it);
    refreshLinks();
    refreshLiteralEditors();
    setDirty(true);
    return true;
}

GraphPinItem* GraphEditorPanel::pin(int node, std::string_view name, bool output) const {
    const auto it = nodes_.find(node);
    return it != nodes_.end() ? it->second->pin(name, output) : nullptr;
}

bool GraphEditorPanel::connectPins(int fromNode, std::string_view fromPin, int toNode, std::string_view toPin) {
    GraphPinItem* out = pin(fromNode, fromPin, true);
    GraphPinItem* in = pin(toNode, toPin, false);
    if (out == nullptr || in == nullptr || fromNode == toNode) return false;
    if (!compatible(static_cast<int>(out->def().type), static_cast<int>(in->def().type))) return false;
    const bool exec = out->def().type == PinType::Exec;
    for (auto l = links_.begin(); l != links_.end();) {
        const bool same = l->fromNode == fromNode && l->fromPin == fromPin && l->toNode == toNode && l->toPin == toPin;
        if (same) return true;
        if (!exec && l->toNode == toNode && l->toPin == toPin) {  // a data input has one source
            delete l->item;
            l = links_.erase(l);
        } else {
            ++l;
        }
    }
    links_.push_back({fromNode, std::string(fromPin), toNode, std::string(toPin), nullptr});
    rebuildLinkItem(links_.back());
    refreshLiteralEditors();
    setDirty(true);
    return true;
}

bool GraphEditorPanel::disconnectInput(int node, std::string_view name) {
    const auto before = links_.size();
    for (auto l = links_.begin(); l != links_.end();) {
        if (l->toNode == node && l->toPin == name) {
            delete l->item;
            l = links_.erase(l);
        } else {
            ++l;
        }
    }
    if (links_.size() == before) return false;
    refreshLiteralEditors();
    setDirty(true);
    return true;
}

int GraphEditorPanel::disconnectPin(int node, std::string_view name, bool output) {
    const auto removed = std::erase_if(links_, [&](const Link& l) {
        const bool hit = output ? (l.fromNode == node && l.fromPin == name) : (l.toNode == node && l.toPin == name);
        if (hit) delete l.item;
        return hit;
    });
    if (removed == 0) return 0;
    refreshLiteralEditors();
    setDirty(true);
    return static_cast<int>(removed);
}

int GraphEditorPanel::disconnectNode(int node) {
    const auto removed = std::erase_if(links_, [&](const Link& l) {
        const bool hit = l.fromNode == node || l.toNode == node;
        if (hit) delete l.item;
        return hit;
    });
    if (removed == 0) return 0;
    refreshLiteralEditors();
    setDirty(true);
    return static_cast<int>(removed);
}

bool GraphEditorPanel::removeLink(int fromNode, std::string_view fromPin, int toNode, std::string_view toPin) {
    const auto removed = std::erase_if(links_, [&](const Link& l) {
        const bool hit = l.fromNode == fromNode && l.fromPin == fromPin && l.toNode == toNode && l.toPin == toPin;
        if (hit) delete l.item;
        return hit;
    });
    if (removed == 0) return false;
    refreshLiteralEditors();
    setDirty(true);
    return true;
}

int GraphEditorPanel::duplicateNode(int id) {
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) return 0;
    const int copy = nextId_++;
    auto* item = new GraphNodeItem(*this, copy, it->second->def(), it->second->literals());
    scene_->addItem(item);
    item->setPos(it->second->pos() + QPointF(30, 30));
    nodes_[copy] = item;
    refreshLiteralEditors();
    scene_->clearSelection();
    item->setSelected(true);
    setDirty(true);
    return copy;
}

QMenu* GraphEditorPanel::contextMenuAt(QPointF scenePosition, QWidget* parent) {
    GraphPinItem* pinItem = nullptr;
    GraphNodeItem* nodeItem = nullptr;
    GraphLinkItem* linkItem = nullptr;
    // Pins are small: look a few pixels around the click for one first.
    for (QGraphicsItem* item : scene_->items(QRectF(scenePosition - QPointF(4, 4), QSizeF(8, 8)))) {
        if (auto* p = dynamic_cast<GraphPinItem*>(item)) {
            pinItem = p;
            break;
        }
    }
    if (pinItem == nullptr) {
        for (QGraphicsItem* item : scene_->items(scenePosition)) {
            for (QGraphicsItem* i = item; i != nullptr && nodeItem == nullptr; i = i->parentItem()) {
                nodeItem = dynamic_cast<GraphNodeItem*>(i);
            }
            if (nodeItem != nullptr) break;
            if ((linkItem = dynamic_cast<GraphLinkItem*>(item)) != nullptr) break;
        }
    }
    if (pinItem == nullptr && nodeItem == nullptr && linkItem == nullptr) return nullptr;

    auto* menu = new QMenu(parent);
    if (pinItem != nullptr) {
        const int node = pinItem->node();
        const std::string name(pinItem->def().name);
        const bool output = pinItem->output();
        QAction* cut = menu->addAction(app::theme::icon(QStringLiteral("close")), tr("Break Links"),
                                       [this, node, name, output] { disconnectPin(node, name, output); });
        cut->setEnabled(pinItem->linked);
        menu->addAction(tr("Delete Node"), [this, node] { removeNode(node); });
    } else if (nodeItem != nullptr) {
        const int id = nodeItem->id();
        // On a node that is part of a bigger selection, deleting takes the whole selection.
        const qsizetype selected = scene_->selectedItems().size();
        if (nodeItem->isSelected() && selected > 1) {
            menu->addAction(app::theme::icon(QStringLiteral("trash")), tr("Delete %1 Selected").arg(selected),
                            [this] { removeSelection(); });
        } else {
            menu->addAction(app::theme::icon(QStringLiteral("trash")), tr("Delete Node"), [this, id] { removeNode(id); })
                ->setShortcut(QKeySequence::Delete);
        }
        menu->addAction(app::theme::icon(QStringLiteral("plus")), tr("Duplicate"), [this, id] { duplicateNode(id); });
        const bool hasLinks = std::ranges::any_of(links_, [id](const Link& l) { return l.fromNode == id || l.toNode == id; });
        menu->addAction(app::theme::icon(QStringLiteral("close")), tr("Break All Links"), [this, id] { disconnectNode(id); })
            ->setEnabled(hasLinks);
    } else {
        const auto link = std::ranges::find(links_, linkItem, &Link::item);
        if (link == links_.end()) {
            delete menu;
            return nullptr;
        }
        menu->addAction(app::theme::icon(QStringLiteral("close")), tr("Remove Link"),
                        [this, from = link->fromNode, fromPin = link->fromPin, to = link->toNode, toPin = link->toPin] {
                            removeLink(from, fromPin, to, toPin);
                        });
    }
    return menu;
}

void GraphEditorPanel::setLiteral(int node, std::string_view pinName, const nlohmann::json& value) {
    const auto it = nodes_.find(node);
    if (it == nodes_.end()) return;
    it->second->literals()[std::string(pinName)] = value;
    if (!loading_) setDirty(true);
}

void GraphEditorPanel::rebuildLinkItem(Link& link) {
    delete link.item;
    GraphPinItem* out = pin(link.fromNode, link.fromPin, true);
    GraphPinItem* in = pin(link.toNode, link.toPin, false);
    link.item = new GraphLinkItem(out != nullptr ? out->def().type : PinType::Any);
    scene_->addItem(link.item);
    if (out != nullptr && in != nullptr) link.item->setEnds(out->scenePos(), in->scenePos());
}

void GraphEditorPanel::refreshLinks() {
    for (Link& link : links_) {
        GraphPinItem* out = pin(link.fromNode, link.fromPin, true);
        GraphPinItem* in = pin(link.toNode, link.toPin, false);
        if (link.item != nullptr && out != nullptr && in != nullptr) link.item->setEnds(out->scenePos(), in->scenePos());
    }
}

void GraphEditorPanel::refreshLiteralEditors() {
    for (auto& [id, node] : nodes_) {
        for (GraphPinItem* p : node->inputs()) p->linked = false;
        for (GraphPinItem* p : node->outputs()) p->linked = false;
    }
    for (const Link& link : links_) {
        if (GraphPinItem* p = pin(link.toNode, link.toPin, false)) p->linked = true;
        if (GraphPinItem* p = pin(link.fromNode, link.fromPin, true)) p->linked = true;
    }
    for (auto& [id, node] : nodes_) {
        node->refreshEditors();
        node->update();
    }
}

void GraphEditorPanel::removeSelection() {
    const QList<QGraphicsItem*> selected = scene_->selectedItems();
    std::vector<int> nodeIds;
    for (QGraphicsItem* item : selected) {
        if (auto* node = dynamic_cast<GraphNodeItem*>(item)) {
            nodeIds.push_back(node->id());
        } else if (auto* link = dynamic_cast<GraphLinkItem*>(item)) {
            for (auto l = links_.begin(); l != links_.end(); ++l) {
                if (l->item == link) {
                    delete l->item;
                    links_.erase(l);
                    setDirty(true);
                    break;
                }
            }
        }
    }
    for (int id : nodeIds) removeNode(id);
    refreshLiteralEditors();
}

void GraphEditorPanel::pinDragFinished(GraphPinItem* from, QPointF scenePos) {
    GraphPinItem* target = nullptr;
    for (QGraphicsItem* item : scene_->items(QRectF(scenePos - QPointF(10, 10), QSizeF(20, 20)))) {
        if (auto* p = dynamic_cast<GraphPinItem*>(item); p != nullptr && p != from && p->output() != from->output()) {
            target = p;
            break;
        }
    }
    if (target == nullptr) {
        // Dropped on empty canvas: an input pin's link is removed.
        if (!from->output()) disconnectInput(from->node(), from->def().name);
        return;
    }
    GraphPinItem* out = from->output() ? from : target;
    GraphPinItem* in = from->output() ? target : from;
    connectPins(out->node(), out->def().name, in->node(), in->def().name);
}

void GraphEditorPanel::showNodePicker(QPointF scenePosition) {
    if (file_.isEmpty()) return;
    const QPoint global = view_->viewport()->mapToGlobal(view_->mapFromScene(scenePosition));
    const std::string type = pickNode(this, global);
    if (!type.empty()) addNode(type, scenePosition);
}

void GraphEditorPanel::addVariable(const QString& name, const QString& type) {
    const bool wasLoading = loading_;
    loading_ = true;
    const int row = variables_->rowCount();
    variables_->insertRow(row);
    variables_->setItem(row, 0, new QTableWidgetItem(name));
    auto* typeBox = new QComboBox;
    typeBox->addItems({QStringLiteral("number"), QStringLiteral("bool"), QStringLiteral("vec3"), QStringLiteral("string")});
    typeBox->setCurrentText(type);
    connect(typeBox, &QComboBox::currentTextChanged, this, [this] { setDirty(true); });
    variables_->setCellWidget(row, 1, typeBox);
    variables_->setItem(row, 2, new QTableWidgetItem(type == QLatin1String("bool") ? QStringLiteral("false")
                                                     : type == QLatin1String("vec3") ? QStringLiteral("0, 0, 0")
                                                     : type == QLatin1String("string") ? QString()
                                                                                       : QStringLiteral("0")));
    loading_ = wasLoading;
    if (!loading_) setDirty(true);
    for (auto& [id, node] : nodes_) node->reloadVariables(variableNames());
}

QStringList GraphEditorPanel::variableNames() const {
    QStringList out;
    for (int row = 0; row < variables_->rowCount(); ++row) {
        if (const QTableWidgetItem* item = variables_->item(row, 0); item != nullptr && !item->text().trimmed().isEmpty()) {
            out << item->text().trimmed();
        }
    }
    return out;
}

void GraphEditorPanel::loadVariables(const nlohmann::json& variables) {
    if (!variables.is_array()) return;
    for (const auto& v : variables) {
        if (!v.is_object() || !v.contains("name") || !v["name"].is_string()) continue;
        const QString type = QString::fromStdString(v.value("type", std::string("number")));
        addVariable(QString::fromStdString(v["name"].get<std::string>()), type);
        const int row = variables_->rowCount() - 1;
        const auto value = visual::Value::fromJson(v.contains("value") ? v["value"] : nlohmann::json(),
                                                   visual::pinTypeFromName(type.toStdString()));
        QString text = QString::fromStdString(value.asString());
        if (value.type == PinType::Vec3) text = QStringLiteral("%1, %2, %3").arg(value.vec.x).arg(value.vec.y).arg(value.vec.z);
        variables_->item(row, 2)->setText(text);
    }
}

nlohmann::json GraphEditorPanel::variablesJson() const {
    nlohmann::json out = nlohmann::json::array();
    for (int row = 0; row < variables_->rowCount(); ++row) {
        const QTableWidgetItem* nameItem = variables_->item(row, 0);
        if (nameItem == nullptr || nameItem->text().trimmed().isEmpty()) continue;
        const auto* typeBox = qobject_cast<QComboBox*>(variables_->cellWidget(row, 1));
        const std::string type = typeBox != nullptr ? typeBox->currentText().toStdString() : "number";
        const QTableWidgetItem* valueItem = variables_->item(row, 2);
        const std::string text = valueItem != nullptr ? valueItem->text().toStdString() : std::string();
        const visual::Value raw = visual::Value::ofString(text);
        nlohmann::json value;
        if (type == "bool") value = raw.asBool();
        else if (type == "vec3") value = visual::Value::ofVec3(raw.asVec3()).toJson();
        else if (type == "string") value = text;
        else value = raw.asNumber();
        out.push_back({{"name", nameItem->text().trimmed().toStdString()}, {"type", type}, {"value", value}});
    }
    return out;
}

nlohmann::json GraphEditorPanel::document() const {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& [id, node] : nodes_) {
        nodes.push_back({{"id", id},
                         {"type", std::string(node->def().type)},
                         {"x", std::round(node->pos().x())},
                         {"y", std::round(node->pos().y())},
                         {"inputs", node->literals()}});
    }
    nlohmann::json links = nlohmann::json::array();
    for (const Link& l : links_) {
        links.push_back({{"from", l.fromNode}, {"fromPin", l.fromPin}, {"to", l.toNode}, {"toPin", l.toPin}});
    }
    return {{"version", 1}, {"variables", variablesJson()}, {"nodes", nodes}, {"links", links}};
}

bool GraphEditorPanel::open(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const nlohmann::json doc = nlohmann::json::parse(f.readAll().toStdString(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        QMessageBox::warning(this, tr("Visual Script"), tr("%1 is not a valid visual script.").arg(QFileInfo(path).fileName()));
        return false;
    }
    loading_ = true;
    clear();
    file_ = path;
    loadVariables(doc.value("variables", nlohmann::json::array()));
    for (const auto& n : doc.value("nodes", nlohmann::json::array())) {
        if (!n.is_object()) continue;
        const NodeDef* def = visual::findNode(n.value("type", std::string()));
        const int id = n.value("id", 0);
        if (def == nullptr || id <= 0 || nodes_.contains(id)) continue;
        auto* item = new GraphNodeItem(*this, id, *def, n.value("inputs", nlohmann::json::object()));
        scene_->addItem(item);
        item->setPos(n.value("x", 0.0), n.value("y", 0.0));
        nodes_[id] = item;
        nextId_ = std::max(nextId_, id + 1);
    }
    for (const auto& l : doc.value("links", nlohmann::json::array())) {
        if (!l.is_object()) continue;
        connectPins(l.value("from", 0), l.value("fromPin", std::string()), l.value("to", 0), l.value("toPin", std::string()));
    }
    refreshLiteralEditors();
    loading_ = false;
    dirty_ = true;  // so setDirty(false) updates the title
    setDirty(false);
    QTimer::singleShot(0, this, [this] { fitGraph(); });
    return true;
}

bool GraphEditorPanel::save() {
    if (file_.isEmpty()) return false;
    QFile f(file_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Visual Script"), tr("Could not write %1.").arg(file_));
        return false;
    }
    const std::string text = document().dump(2) + "\n";
    f.write(text.data(), static_cast<qint64>(text.size()));
    setDirty(false);
    return true;
}

} // namespace lumora::editor
