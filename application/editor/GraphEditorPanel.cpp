#include "editor/GraphEditorPanel.hpp"

#include "assets/MaterialGraph.hpp"
#include "common/Theme.hpp"
#include "runtime/ScriptNames.hpp"
#include "runtime/VisualScript.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
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
#include <deque>
#include <format>
#include <memory>

namespace lumora::editor {

using visual::NodeDef;
using visual::PinDef;
using visual::PinType;

namespace {

/// The material graph nodes (engine/assets/MaterialGraph.hpp) as editor node definitions: pure
/// nodes, numbers, vectors, a texture's asset key; link-only inputs (UV, position) get no field.
const std::vector<NodeDef>& materialNodeLibrary() {
    static std::deque<std::string> texts;  // vector defaults as "x,y,z" (string_views point here)
    static const std::vector<NodeDef> library = [] {
        std::vector<NodeDef> out;
        auto pin = [](const matgraph::Pin& p) {
            PinDef d;
            d.name = p.name;
            d.label = p.label;
            switch (p.kind) {
            case matgraph::PinKind::Float: d.type = PinType::Number; d.defaultNumber = p.x; break;
            case matgraph::PinKind::Bool: d.type = PinType::Bool; d.defaultNumber = p.x; break;
            case matgraph::PinKind::Texture: d.type = PinType::String; d.hint = visual::PinHint::Asset; break;
            case matgraph::PinKind::Vector:
                d.type = PinType::Vec3;
                if (p.defaultText == "auto") {
                    d.defaultText = "auto";
                } else {
                    texts.push_back(std::format("{},{},{}", p.x, p.y, p.z));
                    d.defaultText = texts.back();
                }
                break;
            }
            return d;
        };
        for (const matgraph::NodeType& t : matgraph::nodeTypes()) {
            NodeDef d;
            d.type = t.type;
            d.title = t.title;
            d.category = t.category;
            d.description = t.description;
            d.pure = true;
            for (const matgraph::Pin& p : t.inputs) d.inputs.push_back(pin(p));
            for (const matgraph::Pin& p : t.outputs) d.outputs.push_back(pin(p));
            out.push_back(std::move(d));
        }
        return out;
    }();
    return library;
}

} // namespace

namespace {

constexpr qreal kNodeWidth = 300.0;
constexpr qreal kHeader = 26.0;
constexpr qreal kRow = 26.0;
constexpr qreal kPinRadius = 6.0;
constexpr qreal kRerouteSize = 18.0;
constexpr qreal kCommentHeader = 28.0;
const QColor kLive(0xff, 0xae, 0x3c);  // what is running right now
constexpr const char* kClipboardMime = "application/x-lumora-graph";
constexpr const char* kVariableMime = "application/x-lumora-graph-variable";

bool isReroute(std::string_view type) { return type == "flow.reroute" || type == "data.reroute"; }

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
        p->setBrush(Qt::NoBrush);
        if (glow_ > 0.0f) {  // it fired lately: a warm halo that fades
            QColor halo = kLive;
            halo.setAlphaF(0.55f * glow_);
            p->setPen(QPen(halo, pen.widthF() + 5.0, Qt::SolidLine, Qt::RoundCap));
            p->drawPath(path());
            pen.setColor(QColor::fromRgbF(1.0f, 0.85f + 0.15f * (1.0f - glow_), 0.6f + 0.4f * (1.0f - glow_)));
        }
        if (isSelected()) pen.setColor(app::theme::kAccent);
        p->setPen(pen);
        p->drawPath(path());
    }
    void setGlow(float glow) {
        if (std::abs(glow - glow_) < 0.01f) return;
        glow_ = glow;
        update();
    }
    [[nodiscard]] QPainterPath shape() const override {
        QPainterPathStroker stroker;
        stroker.setWidth(10);
        return stroker.createStroke(path());
    }

private:
    QColor color_;
    float glow_ = 0.0f;
};

class GraphNodeItem final : public QGraphicsItem {
public:
    GraphNodeItem(GraphEditorPanel& panel, int id, const NodeDef& def, nlohmann::json literals)
        : panel_(panel), id_(id), def_(def), literals_(std::move(literals)) {
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        compact_ = isReroute(def.type);
        setToolTip(compact_ ? QObject::tr("Reroute: drag it to bend the link") : qs(def.description));
        if (!literals_.is_object()) literals_ = nlohmann::json::object();
        const int rows = static_cast<int>(std::max(def.inputs.size(), def.outputs.size()));
        width_ = compact_ ? kRerouteSize : kNodeWidth;
        height_ = compact_ ? kRerouteSize : kHeader + std::max(rows, 1) * kRow + 6;
        for (std::size_t i = 0; i < def.inputs.size(); ++i) {
            auto* pin = new GraphPinItem(panel, id, def.inputs[i], false, this);
            pin->setPos(compact_ ? QPointF(2, height_ / 2) : QPointF(0, kHeader + static_cast<qreal>(i) * kRow + kRow / 2));
            inputs_.push_back(pin);
            editors_.push_back(compact_ ? nullptr : createEditor(def.inputs[i], static_cast<int>(i)));
        }
        for (std::size_t i = 0; i < def.outputs.size(); ++i) {
            auto* pin = new GraphPinItem(panel, id, def.outputs[i], true, this);
            pin->setPos(compact_ ? QPointF(width_ - 2, height_ / 2)
                                 : QPointF(kNodeWidth, kHeader + static_cast<qreal>(i) * kRow + kRow / 2));
            outputs_.push_back(pin);
        }
    }

    [[nodiscard]] QRectF boundingRect() const override {
        const qreal values = values_.empty() ? 0.0 : 170.0;  // live value labels right of the outputs
        const qreal halo = 5.0;
        const qreal above = values_.empty() ? halo : 24.0;
        return {-halo, -above, width_ + 2 * halo + values, height_ + halo + above};
    }
    [[nodiscard]] QPainterPath shape() const override {
        QPainterPath path;
        path.addRoundedRect(QRectF(0, 0, width_, height_), 6, 6);
        return path;
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF body(0, 0, width_, height_);
        if (glow_ > 0.0f) {
            QColor halo = kLive;
            halo.setAlphaF(0.85f * glow_);
            p->setPen(QPen(halo, 3));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(body.adjusted(-3, -3, 3, 3), 8, 8);
        }
        paintValues(p);
        if (compact_) {
            p->setPen(QPen(isSelected() ? app::theme::kAccent : app::theme::kBorder, isSelected() ? 2 : 1));
            p->setBrush(QColor(0x2a, 0x2d, 0x34));
            p->drawRoundedRect(body, kRerouteSize / 2, kRerouteSize / 2);
            return;
        }
        p->setPen(QPen(isSelected() ? app::theme::kAccent : app::theme::kBorder, isSelected() ? 2 : 1));
        p->setBrush(QColor(0x22, 0x25, 0x2b, 235));
        p->drawRoundedRect(body, 6, 6);
        QPainterPath header;
        header.addRoundedRect(QRectF(0, 0, width_, kHeader), 6, 6);
        p->fillPath(header, categoryColor(def_.category));
        p->setPen(Qt::white);
        QFont f = p->font();
        f.setBold(true);
        p->setFont(f);
        p->drawText(QRectF(10, 0, width_ - 20, kHeader), Qt::AlignVCenter | Qt::AlignLeft, qs(def_.title));
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
            p->drawText(QRectF(width_ - 92, kHeader + static_cast<qreal>(i) * kRow, 80, kRow),
                        Qt::AlignVCenter | Qt::AlignRight, label(def_.outputs[i]));
        }
    }

    /// Live view: how lit the node is and the values on its outputs (empty = not playing).
    void setLive(float glow, std::map<int, QString> values) {
        if (values.empty() != values_.empty()) prepareGeometryChange();
        const bool same = std::abs(glow - glow_) < 0.01f && values == values_;
        glow_ = glow;
        values_ = std::move(values);
        if (!same) update();
    }
    [[nodiscard]] float glow() const { return glow_; }
    [[nodiscard]] QString value(int pin) const {
        const auto it = values_.find(pin);
        return it != values_.end() ? it->second : QString();
    }
    [[nodiscard]] qreal width() const { return width_; }

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
            if (!panel_.loading_) panel_.changed();
        }
        return QGraphicsItem::itemChange(change, value);
    }

private:
    QGraphicsProxyWidget* createEditor(const PinDef& pin, int index) {
        if (pin.type == PinType::Exec || pin.defaultText == "auto") return nullptr;  // link-only (material UV, position)
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

    void paintValues(QPainter* p) const {
        if (values_.empty()) return;
        QFont f = p->font();
        f.setPixelSize(10);
        p->setFont(f);
        const QFontMetricsF metrics(f);
        for (const auto& [pin, text] : values_) {
            const qreal y = compact_ ? height_ / 2 : kHeader + static_cast<qreal>(pin) * kRow + kRow / 2;
            const QString shown = metrics.elidedText(text, Qt::ElideRight, 150);
            // Just above the pin's link, so it does not cover it.
            const QRectF pill(width_ + 8, y - 21, metrics.horizontalAdvance(shown) + 12, 16);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0x14, 0x16, 0x1a, 225));
            p->drawRoundedRect(pill, 8, 8);
            p->setPen(kLive.lighter(130));
            p->drawText(pill, Qt::AlignCenter, shown);
        }
    }

    GraphEditorPanel& panel_;
    int id_;
    const NodeDef& def_;
    nlohmann::json literals_;
    bool compact_ = false;
    qreal width_ = kNodeWidth;
    float glow_ = 0.0f;
    std::map<int, QString> values_;  // output pin index -> live value
    qreal height_ = 0.0;
    std::vector<GraphPinItem*> inputs_;
    std::vector<GraphPinItem*> outputs_;
    std::vector<QGraphicsProxyWidget*> editors_;  // per input (null for exec pins)
};

/// A titled frame behind nodes. Drag the title to move it with the nodes inside, the corner to
/// resize it; double-click the title to rename it.
class GraphCommentItem final : public QGraphicsItem {
public:
    GraphCommentItem(GraphEditorPanel& panel, QSizeF size, QString text, QColor color)
        : panel_(panel), size_(size), text_(std::move(text)), color_(color) {
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        setZValue(-3);
        setAcceptHoverEvents(true);
    }
    [[nodiscard]] QRectF boundingRect() const override { return QRectF(QPointF(0, 0), size_).adjusted(-2, -2, 2, 2); }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF body(QPointF(0, 0), size_);
        QColor fill = color_;
        fill.setAlpha(38);
        p->setPen(QPen(isSelected() ? app::theme::kAccent : color_.darker(110), isSelected() ? 2 : 1));
        p->setBrush(fill);
        p->drawRoundedRect(body, 8, 8);
        QPainterPath header;
        header.addRoundedRect(QRectF(0, 0, size_.width(), kCommentHeader), 8, 8);
        QColor band = color_;
        band.setAlpha(170);
        p->fillPath(header, band);
        QFont f = p->font();
        f.setBold(true);
        f.setPixelSize(14);
        p->setFont(f);
        p->setPen(Qt::white);
        p->drawText(QRectF(12, 0, size_.width() - 24, kCommentHeader), Qt::AlignVCenter | Qt::AlignLeft, text_);
        // Resize grip.
        p->setPen(QPen(QColor(255, 255, 255, 90), 1.5));
        for (int i = 1; i <= 3; ++i) {
            const qreal d = 4.0 * i;
            p->drawLine(QPointF(size_.width() - d, size_.height() - 2), QPointF(size_.width() - 2, size_.height() - d));
        }
    }

    [[nodiscard]] const QString& text() const { return text_; }
    [[nodiscard]] QColor color() const { return color_; }
    [[nodiscard]] QSizeF size() const { return size_; }
    void setText(const QString& text) {
        text_ = text;
        update();
        panel_.changed();
    }
    void setColor(QColor color) {
        color_ = color;
        update();
        panel_.changed();
    }
    void rename() {
        bool ok = false;
        const QString text = QInputDialog::getText(nullptr, QObject::tr("Comment"), QObject::tr("Text"), QLineEdit::Normal, text_, &ok);
        if (ok) setText(text);
    }

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override {
        setCursor(inGrip(event->pos()) ? Qt::SizeFDiagCursor : Qt::ArrowCursor);
    }
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && inGrip(event->pos())) {
            resizing_ = true;
            event->accept();
            return;
        }
        // The nodes and comments inside move along.
        carried_.clear();
        const QRectF area = sceneBoundingRect();
        for (QGraphicsItem* item : scene()->items(area, Qt::ContainsItemShape)) {
            if (item != this && item->parentItem() == nullptr && !item->isSelected() &&
                (dynamic_cast<GraphNodeItem*>(item) != nullptr || dynamic_cast<GraphCommentItem*>(item) != nullptr)) {
                carried_.push_back(item);
            }
        }
        lastPos_ = pos();
        QGraphicsItem::mousePressEvent(event);
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (resizing_) {
            prepareGeometryChange();
            size_ = QSizeF(std::max(160.0, event->pos().x()), std::max(80.0, event->pos().y()));
            update();
            return;
        }
        QGraphicsItem::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (resizing_) {
            resizing_ = false;
            panel_.changed();
            return;
        }
        carried_.clear();
        QGraphicsItem::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override {
        if (event->pos().y() <= kCommentHeader) rename();
    }
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        if (change == ItemPositionHasChanged) {
            const QPointF delta = pos() - lastPos_;
            lastPos_ = pos();
            for (QGraphicsItem* item : carried_) item->moveBy(delta.x(), delta.y());
            if (!panel_.loading_) panel_.changed();
        }
        return QGraphicsItem::itemChange(change, value);
    }

private:
    [[nodiscard]] bool inGrip(QPointF at) const { return at.x() > size_.width() - 16 && at.y() > size_.height() - 16; }
    GraphEditorPanel& panel_;
    QSizeF size_;
    QString text_;
    QColor color_;
    bool resizing_ = false;
    QPointF lastPos_;
    std::vector<QGraphicsItem*> carried_;
};

// ---- Node picker ------------------------------------------------------------------------------

namespace {

/// Search box over the node library; returns the chosen node type (empty if cancelled).
/// `accept` narrows the list (nodes that fit a dragged pin).
std::string pickNode(QWidget* parent, QPoint globalPos, const std::vector<NodeDef>& library,
                     const std::function<bool(const NodeDef&)>& accept = {}) {
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
        for (const NodeDef& d : library) {
            if (isReroute(d.type) || (accept && !accept(d))) continue;  // reroutes come from double-clicking a link
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
    auto choose = [&] {
        if (QListWidgetItem* item = list->currentItem()) {
            chosen = item->data(Qt::UserRole).toString().toStdString();
            dialog.accept();
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, fill);
    QObject::connect(search, &QLineEdit::returnPressed, &dialog, choose);
    QObject::connect(list, &QListWidget::itemActivated, &dialog, choose);
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

namespace {
bool editingField(const QGraphicsScene* scene) {
    return scene->focusItem() != nullptr && dynamic_cast<QGraphicsProxyWidget*>(scene->focusItem()) != nullptr;
}
/// The graph's own shortcuts; they win over the editor window's (scene undo, copy, ...).
bool graphShortcut(const QKeyEvent* event) {
    const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
    switch (event->key()) {
    case Qt::Key_Z: case Qt::Key_Y: case Qt::Key_C: case Qt::Key_X: case Qt::Key_V: case Qt::Key_D: case Qt::Key_A:
        return ctrl || event->key() == Qt::Key_C;
    case Qt::Key_Delete: case Qt::Key_Backspace: case Qt::Key_Space:
        return !ctrl;
    default:
        return false;
    }
}
} // namespace

bool GraphView::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride && !editingField(scene()) && graphShortcut(static_cast<QKeyEvent*>(event))) {
        event->accept();  // delivered as a key press below instead of triggering a window action
        return true;
    }
    return QGraphicsView::event(event);
}

void GraphView::keyPressEvent(QKeyEvent* event) {
    // Keys typed into a node's field stay there.
    if (editingField(scene())) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
    const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
    if (ctrl) {
        switch (event->key()) {
        case Qt::Key_Z: shift ? panel_.redo() : panel_.undo(); return;
        case Qt::Key_Y: panel_.redo(); return;
        case Qt::Key_C: panel_.copySelection(); return;
        case Qt::Key_X: panel_.cutSelection(); return;
        case Qt::Key_V: {
            const QPoint cursor = mapFromGlobal(QCursor::pos());
            if (rect().contains(cursor)) {
                panel_.paste(mapToScene(cursor));
            } else {
                panel_.paste();
            }
            return;
        }
        case Qt::Key_D: panel_.duplicateSelection(); return;
        case Qt::Key_A: panel_.selectAll(); return;
        default: break;
        }
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        panel_.removeSelection();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        panel_.showNodePicker(mapToScene(mapFromGlobal(QCursor::pos())));
        return;
    }
    if (event->key() == Qt::Key_C && !ctrl) {
        panel_.commentSelection();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void GraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-clicking a link puts a reroute dot there.
    const QPointF at = mapToScene(event->pos());
    for (QGraphicsItem* item : items(event->pos())) {
        if (auto* link = dynamic_cast<GraphLinkItem*>(item)) {
            for (const auto& l : panel_.links_) {
                if (l.item == link) {
                    const auto copy = l;  // insertReroute replaces the link
                    panel_.insertReroute(copy.fromNode, copy.fromPin, copy.toNode, copy.toPin, at);
                    return;
                }
            }
        }
        if (dynamic_cast<GraphLinkItem*>(item) == nullptr) break;  // something else is on top
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void GraphView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(QLatin1String(kVariableMime))) {
        event->acceptProposedAction();
        return;
    }
    QGraphicsView::dragEnterEvent(event);
}

void GraphView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat(QLatin1String(kVariableMime))) {
        event->acceptProposedAction();
        return;
    }
    QGraphicsView::dragMoveEvent(event);
}

void GraphView::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasFormat(QLatin1String(kVariableMime))) {
        const QString name = QString::fromUtf8(event->mimeData()->data(QLatin1String(kVariableMime)));
        event->acceptProposedAction();
        panel_.dropVariable(name, mapToScene(event->position().toPoint()), event->modifiers());
        return;
    }
    QGraphicsView::dropEvent(event);
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

GraphEditorPanel::GraphEditorPanel(QString projectRoot, GraphKind kind, QWidget* parent)
    : QWidget(parent), root_(std::move(projectRoot)), kind_(kind) {
    const bool material = kind_ == GraphKind::Material;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    title_ = new QLabel(material ? tr("No material graph open — create one in the Content Browser (New > Material Graph)")
                                 : tr("No visual script open — create one in the Content Browser (New > Visual Script)"));
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
    auto* undoButton = new QToolButton;
    undoButton->setIcon(app::theme::icon(QStringLiteral("undo")));
    undoButton->setToolTip(tr("Undo (Ctrl+Z)"));
    auto* redoButton = new QToolButton;
    redoButton->setIcon(app::theme::icon(QStringLiteral("redo")));
    redoButton->setToolTip(tr("Redo (Ctrl+Shift+Z)"));
    auto* comment = new QToolButton;
    comment->setText(tr("Comment"));
    comment->setToolTip(tr("Frame the selected nodes in a comment (C)"));
    liveLabel_ = new QLabel;
    liveLabel_->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(kLive.name()));
    liveLabel_->hide();
    bar->addWidget(title_, 1);
    bar->addWidget(liveLabel_);
    bar->addWidget(undoButton);
    bar->addWidget(redoButton);
    bar->addWidget(add);
    bar->addWidget(comment);
    bar->addWidget(fit);
    bar->addWidget(saveButton);
    layout->addLayout(bar);
    connect(undoButton, &QToolButton::clicked, this, &GraphEditorPanel::undo);
    connect(redoButton, &QToolButton::clicked, this, &GraphEditorPanel::redo);
    connect(comment, &QToolButton::clicked, this, &GraphEditorPanel::commentSelection);

    scene_ = new QGraphicsScene(this);
    scene_->setSceneRect(-5000, -5000, 10000, 10000);
    view_ = new GraphView(scene_, *this);

    auto* side = new QWidget;
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    if (material) {
        // A sphere wearing the material and whether it compiles, instead of variables.
        preview_ = new QLabel;
        preview_->setObjectName(QStringLiteral("materialGraphPreview"));
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setMinimumSize(180, 180);
        sideLayout->addWidget(preview_);
        status_ = new QLabel;
        status_->setObjectName(QStringLiteral("materialGraphStatus"));
        status_->setWordWrap(true);
        sideLayout->addWidget(status_);
    }
    auto* variablesLabel = new QLabel(tr("Variables"));
    sideLayout->addWidget(variablesLabel);
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
    auto* help = new QLabel(material ? tr("Link nodes into the Material Output: textures, UVs, colours, math, layers and masks "
                                          "per pixel; World Offset (wind) per vertex. Changes are saved and shown at once. "
                                          "Space / right-click: add node. C: comment. Ctrl+Z/C/V/D: undo, copy, paste, duplicate. "
                                          "Middle mouse: pan, wheel: zoom.")
                                     : tr("Drag from a pin to another pin to link them, or to empty canvas for nodes that fit. "
                               "Space / right-click: add node. Right-click a node, pin or link to delete it or cut links. "
                               "Double-click a link: reroute dot. C: comment. Ctrl+Z/C/V/D: undo, copy, paste, duplicate. "
                               "Drag a variable here for Get/Set (Ctrl: Get, Alt: Set). Middle mouse: pan, wheel: zoom."));
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
        changed();
        for (auto& [id, node] : nodes_) node->reloadVariables(variableNames());
    });
    connect(variables_, &QTableWidget::cellChanged, this, [this] {
        if (loading_) return;
        changed();
        for (auto& [id, node] : nodes_) node->reloadVariables(variableNames());
    });
    // Dragging a variable's row onto the canvas.
    struct VariableDrag final : QObject {
        QTableWidget* table;
        QPoint pressed{-1, -1};
        explicit VariableDrag(QTableWidget* t) : QObject(t), table(t) {}
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::MouseButtonPress) {
                pressed = static_cast<QMouseEvent*>(e)->pos();
            } else if (e->type() == QEvent::MouseMove && (static_cast<QMouseEvent*>(e)->buttons() & Qt::LeftButton) &&
                       pressed.x() >= 0 &&
                       (static_cast<QMouseEvent*>(e)->pos() - pressed).manhattanLength() >= QApplication::startDragDistance()) {
                const QModelIndex index = table->indexAt(pressed);
                pressed = {-1, -1};
                const QTableWidgetItem* name = index.isValid() ? table->item(index.row(), 0) : nullptr;
                if (name == nullptr || name->text().trimmed().isEmpty()) return false;
                auto* drag = new QDrag(table);
                auto* mime = new QMimeData;
                mime->setData(QLatin1String(kVariableMime), name->text().trimmed().toUtf8());
                drag->setMimeData(mime);
                drag->exec(Qt::CopyAction);
                return true;
            } else if (e->type() == QEvent::MouseButtonRelease) {
                pressed = {-1, -1};
            }
            return false;
        }
    };
    variables_->viewport()->installEventFilter(new VariableDrag(variables_));

    commitTimer_ = new QTimer(this);
    commitTimer_->setSingleShot(true);
    commitTimer_->setInterval(400);  // edits within this time (a drag, typing) are one undo step
    connect(commitTimer_, &QTimer::timeout, this, &GraphEditorPanel::commitUndo);
    liveTimer_ = new QTimer(this);
    liveTimer_->setInterval(100);
    connect(liveTimer_, &QTimer::timeout, this, &GraphEditorPanel::refreshLive);
    if (material) {
        variablesLabel->hide();
        variables_->hide();
        addVar->hide();
        removeVar->hide();
        // Saved shortly after each change, like the material editor.
        autoSave_ = new QTimer(this);
        autoSave_->setSingleShot(true);
        autoSave_->setInterval(300);
        connect(autoSave_, &QTimer::timeout, this, [this] {
            if (!file_.isEmpty() && dirty_) save();
        });
    } else {
        liveTimer_->start();
    }
    setEnabled(true);
}

void GraphEditorPanel::setMaterialPreview(std::function<QImage(const QString&, int)> preview) {
    previewRenderer_ = std::move(preview);
    materialChanged();
}

QImage GraphEditorPanel::previewImage() const {
    return preview_ != nullptr ? preview_->pixmap().toImage() : QImage();
}

void GraphEditorPanel::materialChanged() {
    if (kind_ != GraphKind::Material || file_.isEmpty()) return;
    auto program = compileMaterialGraph(document());
    compileError_ = program ? QString() : QString::fromStdString(program.error().message);
    if (status_ != nullptr) {
        status_->setText(program ? tr("%n step(s), %1 texture(s)", "", static_cast<int>(program->fragmentInstructions()))
                                       .arg(program->textures.size()) +
                                       (program->vertex.empty() ? QString() : tr(", moves vertices"))
                                 : tr("Does not compile: %1").arg(compileError_));
        status_->setStyleSheet(program ? QStringLiteral("color: %1;").arg(app::theme::kTextMuted.name())
                                       : QStringLiteral("color: #e06c75;"));
    }
    if (preview_ != nullptr && previewRenderer_ && program) {
        const QImage image = previewRenderer_(file_, 180);
        if (!image.isNull()) preview_->setPixmap(QPixmap::fromImage(image));
    }
}

GraphEditorPanel::~GraphEditorPanel() = default;

// ---- Undo -----------------------------------------------------------------------------------------

void GraphEditorPanel::changed() {
    if (loading_ || restoring_) return;
    setDirty(true);
    if (autoSave_ != nullptr) autoSave_->start();
    commitTimer_->start();
}

void GraphEditorPanel::commitUndo() {
    commitTimer_->stop();
    if (file_.isEmpty()) return;
    nlohmann::json now = document();
    if (now == committed_) return;
    undo_.push_back(std::move(committed_));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    committed_ = std::move(now);
    redo_.clear();
}

bool GraphEditorPanel::canUndo() const { return !undo_.empty() || commitTimer_->isActive(); }
bool GraphEditorPanel::canRedo() const { return !redo_.empty(); }

void GraphEditorPanel::undo() {
    if (commitTimer_->isActive()) commitUndo();  // the edit in progress is a step too
    if (undo_.empty()) return;
    redo_.push_back(std::move(committed_));
    committed_ = std::move(undo_.back());
    undo_.pop_back();
    restore(committed_);
}

void GraphEditorPanel::redo() {
    if (commitTimer_->isActive()) commitUndo();
    if (redo_.empty()) return;
    undo_.push_back(std::move(committed_));
    committed_ = std::move(redo_.back());
    redo_.pop_back();
    restore(committed_);
}

void GraphEditorPanel::restore(const nlohmann::json& doc) {
    restoring_ = true;
    loadDocument(doc);
    restoring_ = false;
    setDirty(document() != saved_);
    if (autoSave_ != nullptr && dirty_) autoSave_->start();  // material graphs: undo is saved too
}

// ---- Clipboard ------------------------------------------------------------------------------------

std::vector<int> GraphEditorPanel::selectedNodes() const {
    std::vector<int> ids;
    for (QGraphicsItem* item : scene_->selectedItems()) {
        if (auto* node = dynamic_cast<GraphNodeItem*>(item)) ids.push_back(node->id());
    }
    std::ranges::sort(ids);
    return ids;
}

void GraphEditorPanel::selectNodes(const std::vector<int>& ids) {
    scene_->clearSelection();
    for (int id : ids) {
        if (const auto it = nodes_.find(id); it != nodes_.end()) it->second->setSelected(true);
    }
}

void GraphEditorPanel::selectAll() {
    for (QGraphicsItem* item : scene_->items()) {
        if (item->flags() & QGraphicsItem::ItemIsSelectable) item->setSelected(true);
    }
}

nlohmann::json GraphEditorPanel::selectionDocument() const {
    nlohmann::json nodes = nlohmann::json::array();
    std::vector<int> ids = selectedNodes();
    for (int id : ids) {
        const GraphNodeItem* node = nodes_.at(id);
        nodes.push_back({{"id", id}, {"type", std::string(node->def().type)}, {"x", node->pos().x()}, {"y", node->pos().y()},
                         {"inputs", const_cast<GraphNodeItem*>(node)->literals()}});
    }
    nlohmann::json links = nlohmann::json::array();
    for (const Link& l : links_) {  // only the links inside the selection
        if (std::ranges::binary_search(ids, l.fromNode) && std::ranges::binary_search(ids, l.toNode)) {
            links.push_back({{"from", l.fromNode}, {"fromPin", l.fromPin}, {"to", l.toNode}, {"toPin", l.toPin}});
        }
    }
    nlohmann::json comments = nlohmann::json::array();
    for (const GraphCommentItem* c : comments_) {
        if (!c->isSelected()) continue;
        comments.push_back({{"x", c->pos().x()}, {"y", c->pos().y()}, {"w", c->size().width()}, {"h", c->size().height()},
                            {"text", c->text().toStdString()}, {"color", c->color().name().toStdString()}});
    }
    return {{"lumoraGraph", 1}, {"nodes", nodes}, {"links", links}, {"comments", comments}};
}

void GraphEditorPanel::copySelection() {
    const nlohmann::json doc = selectionDocument();
    if (doc["nodes"].empty() && doc["comments"].empty()) return;
    auto* mime = new QMimeData;
    const QByteArray bytes = QByteArray::fromStdString(doc.dump());
    mime->setData(QLatin1String(kClipboardMime), bytes);
    mime->setText(QString::fromUtf8(bytes));
    QApplication::clipboard()->setMimeData(mime);
    pasteCount_ = 0;
}

void GraphEditorPanel::cutSelection() {
    copySelection();
    removeSelection();
}

bool GraphEditorPanel::paste(std::optional<QPointF> at) {
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (mime == nullptr) return false;
    const QByteArray bytes = mime->hasFormat(QLatin1String(kClipboardMime)) ? mime->data(QLatin1String(kClipboardMime))
                                                                             : mime->text().toUtf8();
    return pasteDocument(nlohmann::json::parse(bytes.toStdString(), nullptr, false), at, ++pasteCount_);
}

bool GraphEditorPanel::pasteDocument(const nlohmann::json& doc, std::optional<QPointF> at, int step) {
    if (file_.isEmpty() || !doc.is_object() || !doc.contains("lumoraGraph")) return false;
    // Where the copied things go: their top-left at `at`, or a step down-right of the originals.
    QPointF topLeft(1e9, 1e9);
    for (const auto* list : {&doc["nodes"], &doc["comments"]}) {
        for (const auto& n : *list) topLeft = QPointF(std::min(topLeft.x(), n.value("x", 0.0)), std::min(topLeft.y(), n.value("y", 0.0)));
    }
    if (topLeft.x() > 1e8) return false;
    const QPointF offset = at ? *at - topLeft : QPointF(40, 40) * step;
    std::map<int, int> newId;
    std::vector<int> pasted;
    scene_->clearSelection();
    for (const auto& n : doc["nodes"]) {
        const NodeDef* def = findNode(n.value("type", std::string()));
        if (def == nullptr) continue;
        const int id = nextId_++;
        auto* item = new GraphNodeItem(*this, id, *def, n.value("inputs", nlohmann::json::object()));
        scene_->addItem(item);
        item->setPos(QPointF(n.value("x", 0.0), n.value("y", 0.0)) + offset);
        nodes_[id] = item;
        newId[n.value("id", 0)] = id;
        pasted.push_back(id);
    }
    for (const auto& l : doc["links"]) {
        const auto from = newId.find(l.value("from", 0));
        const auto to = newId.find(l.value("to", 0));
        if (from != newId.end() && to != newId.end()) {
            connectPins(from->second, l.value("fromPin", std::string()), to->second, l.value("toPin", std::string()));
        }
    }
    for (const auto& c : doc["comments"]) {
        const int index = addComment(QRectF(QPointF(c.value("x", 0.0), c.value("y", 0.0)) + offset,
                                            QSizeF(c.value("w", 300.0), c.value("h", 200.0))),
                                     QString::fromStdString(c.value("text", std::string())));
        comments_[static_cast<std::size_t>(index)]->setColor(QColor(QString::fromStdString(c.value("color", std::string("#4f8cff")))));
        comments_[static_cast<std::size_t>(index)]->setSelected(true);
    }
    for (int id : pasted) nodes_[id]->setSelected(true);
    refreshLiteralEditors();
    changed();
    return true;
}

void GraphEditorPanel::duplicateSelection() { pasteDocument(selectionDocument(), std::nullopt, 1); }

// ---- Comments and reroutes ------------------------------------------------------------------------

int GraphEditorPanel::addComment(const QRectF& rect, const QString& text) {
    auto* item = new GraphCommentItem(*this, rect.size(), text, app::theme::kAccent);
    scene_->addItem(item);
    item->setPos(rect.topLeft());
    comments_.push_back(item);
    changed();
    return static_cast<int>(comments_.size()) - 1;
}

void GraphEditorPanel::commentSelection() {
    if (file_.isEmpty()) return;
    QRectF bounds;
    for (int id : selectedNodes()) bounds |= nodes_[id]->sceneBoundingRect();
    if (bounds.isNull()) {
        const QPointF center = view_->mapToScene(view_->viewport()->rect().center());
        bounds = QRectF(center - QPointF(200, 120), QSizeF(400, 240));
    } else {
        bounds = bounds.adjusted(-24, -24 - kCommentHeader, 24, 24);
    }
    const int index = addComment(bounds, tr("Comment"));
    scene_->clearSelection();
    comments_[static_cast<std::size_t>(index)]->setSelected(true);
}

void GraphEditorPanel::removeComment(GraphCommentItem* comment) {
    const auto it = std::ranges::find(comments_, comment);
    if (it == comments_.end()) return;
    comments_.erase(it);
    delete comment;
    changed();
}

int GraphEditorPanel::insertReroute(int fromNode, std::string_view fromPin, int toNode, std::string_view toPin, QPointF at) {
    GraphPinItem* out = pin(fromNode, fromPin, true);
    if (out == nullptr || pin(toNode, toPin, false) == nullptr) return 0;
    const bool exec = out->def().type == PinType::Exec;
    const std::string from(fromPin);
    const std::string to(toPin);
    const int dot = addNode(exec ? "flow.reroute" : "data.reroute", at - QPointF(kRerouteSize / 2, kRerouteSize / 2));
    if (dot == 0) return 0;
    removeLink(fromNode, from, toNode, to);
    connectPins(fromNode, from, dot, exec ? "in" : "value");
    connectPins(dot, exec ? "then" : "value", toNode, to);
    return dot;
}

int GraphEditorPanel::addVariableNode(const QString& name, bool set, QPointF at) {
    if (kind_ == GraphKind::Material) return 0;  // no variables in material graphs
    const NodeDef* def = visual::findNode(set ? "var.set" : "var.get");
    if (def == nullptr) return 0;
    const int id = nextId_++;
    auto* item = new GraphNodeItem(*this, id, *def, nlohmann::json{{"name", name.toStdString()}});
    scene_->addItem(item);
    item->setPos(at);
    item->reloadVariables(variableNames());
    nodes_[id] = item;
    changed();
    return id;
}

void GraphEditorPanel::dropVariable(const QString& name, QPointF scenePos, Qt::KeyboardModifiers modifiers) {
    if (modifiers & Qt::ControlModifier) {
        addVariableNode(name, false, scenePos);
    } else if (modifiers & Qt::AltModifier) {
        addVariableNode(name, true, scenePos);
    } else {
        QMenu menu(this);
        menu.addAction(tr("Get %1").arg(name), this, [&] { addVariableNode(name, false, scenePos); });
        menu.addAction(tr("Set %1").arg(name), this, [&] { addVariableNode(name, true, scenePos); });
        menu.exec(QCursor::pos());
    }
}

// ---- Live view --------------------------------------------------------------------------------------

void GraphEditorPanel::refreshLive() {
    std::shared_ptr<visual::GraphTrace> trace = liveKey_.empty() ? nullptr : visual::trace::find(liveKey_);
    bool fresh = false;
    if (trace) {
        const std::lock_guard lock(trace->mutex);
        fresh = std::chrono::steady_clock::now() - trace->updated < std::chrono::milliseconds(600);
    }
    if (!fresh) {
        if (!live_) return;
        live_ = false;
        liveLabel_->hide();
        for (auto& [id, node] : nodes_) node->setLive(0.0f, {});
        for (Link& l : links_) {
            if (l.item != nullptr) l.item->setGlow(0.0f);
        }
        return;
    }
    live_ = true;
    const std::lock_guard lock(trace->mutex);
    liveLabel_->setText(trace->entityName.empty() ? tr("● Live: entity #%1").arg(trace->entity)
                                                  : tr("● Live: %1").arg(QString::fromStdString(trace->entityName)));
    liveLabel_->show();
    constexpr double kFadeSteps = 30.0;  // about half a second at 60 steps per second
    auto glowOf = [&](std::uint64_t last) {
        const double age = static_cast<double>(trace->step - std::min(last, trace->step));
        return static_cast<float>(std::clamp(1.0 - age / kFadeSteps, 0.0, 1.0));
    };
    for (auto& [id, node] : nodes_) {
        const auto ran = trace->nodes.find(id);
        std::map<int, QString> values;
        for (std::size_t pin = 0; pin < node->def().outputs.size(); ++pin) {
            if (const auto v = trace->values.find({id, static_cast<int>(pin)}); v != trace->values.end()) {
                values[static_cast<int>(pin)] = QString::fromStdString(v->second);
            }
        }
        node->setLive(ran != trace->nodes.end() ? glowOf(ran->second) : 0.0f, std::move(values));
    }
    for (Link& l : links_) {
        if (l.item == nullptr) continue;
        const auto from = nodes_.find(l.fromNode);
        if (from == nodes_.end()) continue;
        const int pinIndex = from->second->def().outputIndex(l.fromPin);
        float glow = 0.0f;
        if (const auto fired = trace->links.find({l.fromNode, pinIndex}); fired != trace->links.end()) {
            glow = glowOf(fired->second);
        } else if (const auto ran = trace->nodes.find(l.fromNode); ran != trace->nodes.end() &&
                                                                    from->second->def().outputs[static_cast<std::size_t>(pinIndex)].type != PinType::Exec) {
            glow = 0.6f * glowOf(ran->second);  // a value read lately
        }
        l.item->setGlow(glow);
    }
}

float GraphEditorPanel::nodeGlow(int id) const {
    const auto it = nodes_.find(id);
    return it != nodes_.end() ? it->second->glow() : 0.0f;
}

QString GraphEditorPanel::liveValue(int node, std::string_view pinName) const {
    const auto it = nodes_.find(node);
    if (it == nodes_.end()) return {};
    return it->second->value(it->second->def().outputIndex(pinName));
}

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
    comments_.clear();
    scene_->clear();
    variables_->setRowCount(0);
    nextId_ = 1;
}

bool GraphEditorPanel::canLink(int fromType, int toType) const {
    // Material graphs: a vector into a number takes its first component.
    if (kind_ == GraphKind::Material && fromType == static_cast<int>(PinType::Vec3) && toType == static_cast<int>(PinType::Number)) {
        return true;
    }
    return compatible(fromType, toType);
}

const NodeDef* GraphEditorPanel::findNode(std::string_view type) const {
    if (kind_ == GraphKind::VisualScript) return visual::findNode(type);
    for (const NodeDef& d : materialNodeLibrary()) {
        if (d.type == type) return &d;
    }
    return nullptr;
}

const std::vector<NodeDef>& GraphEditorPanel::library() const {
    return kind_ == GraphKind::Material ? materialNodeLibrary() : visual::nodeLibrary();
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
    const NodeDef* def = findNode(type);
    if (def == nullptr) return 0;
    const int id = nextId_++;
    auto* item = new GraphNodeItem(*this, id, *def, nlohmann::json::object());
    scene_->addItem(item);
    item->setPos(position);
    nodes_[id] = item;
    changed();
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
    changed();
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
    if (!canLink(static_cast<int>(out->def().type), static_cast<int>(in->def().type))) return false;
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
    changed();
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
    changed();
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
    changed();
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
    changed();
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
    changed();
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
    changed();
    return copy;
}

QMenu* GraphEditorPanel::contextMenuAt(QPointF scenePosition, QWidget* parent) {
    GraphPinItem* pinItem = nullptr;
    GraphNodeItem* nodeItem = nullptr;
    GraphLinkItem* linkItem = nullptr;
    GraphCommentItem* commentItem = nullptr;
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
            if ((commentItem = dynamic_cast<GraphCommentItem*>(item)) != nullptr) break;
        }
    }
    if (pinItem == nullptr && nodeItem == nullptr && linkItem == nullptr && commentItem == nullptr) return nullptr;

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
    } else if (linkItem != nullptr) {
        const auto link = std::ranges::find(links_, linkItem, &Link::item);
        if (link == links_.end()) {
            delete menu;
            return nullptr;
        }
        menu->addAction(app::theme::icon(QStringLiteral("close")), tr("Remove Link"),
                        [this, from = link->fromNode, fromPin = link->fromPin, to = link->toNode, toPin = link->toPin] {
                            removeLink(from, fromPin, to, toPin);
                        });
        menu->addAction(tr("Add Reroute Point"),
                        [this, from = link->fromNode, fromPin = link->fromPin, to = link->toNode, toPin = link->toPin, scenePosition] {
                            insertReroute(from, fromPin, to, toPin, scenePosition);
                        });
    } else {
        menu->addAction(app::theme::icon(QStringLiteral("plus")), tr("Add Node Here…"),
                        [this, scenePosition] { showNodePicker(scenePosition); });
        menu->addAction(tr("Rename…"), [commentItem] { commentItem->rename(); });
        QMenu* colors = menu->addMenu(tr("Colour"));
        const std::pair<const char*, QColor> palette[] = {{"Blue", app::theme::kAccent},     {"Green", QColor(0x3f, 0xa8, 0x5c)},
                                                          {"Orange", QColor(0xd9, 0x84, 0x2b)}, {"Red", QColor(0xc8, 0x45, 0x45)},
                                                          {"Purple", QColor(0x8a, 0x5c, 0xc8)}, {"Grey", QColor(0x70, 0x76, 0x82)}};
        for (const auto& [name, color] : palette) {
            QPixmap swatch(12, 12);
            swatch.fill(color);
            colors->addAction(QIcon(swatch), tr(name), [commentItem, color] { commentItem->setColor(color); });
        }
        menu->addAction(app::theme::icon(QStringLiteral("trash")), tr("Delete Comment"),
                        [this, commentItem] { removeComment(commentItem); });
    }
    return menu;
}

void GraphEditorPanel::setLiteral(int node, std::string_view pinName, const nlohmann::json& value) {
    const auto it = nodes_.find(node);
    if (it == nodes_.end()) return;
    it->second->literals()[std::string(pinName)] = value;
    if (!loading_) changed();
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
    std::vector<GraphCommentItem*> comments;
    for (QGraphicsItem* item : selected) {
        if (auto* comment = dynamic_cast<GraphCommentItem*>(item)) {
            comments.push_back(comment);
        } else if (auto* node = dynamic_cast<GraphNodeItem*>(item)) {
            nodeIds.push_back(node->id());
        } else if (auto* link = dynamic_cast<GraphLinkItem*>(item)) {
            for (auto l = links_.begin(); l != links_.end(); ++l) {
                if (l->item == link) {
                    delete l->item;
                    links_.erase(l);
                    changed();
                    break;
                }
            }
        }
    }
    for (int id : nodeIds) removeNode(id);
    for (GraphCommentItem* comment : comments) removeComment(comment);
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
        // Dropped on empty canvas: a linked input loses its link; otherwise pick a node that fits.
        if (!from->output() && from->linked) {
            disconnectInput(from->node(), from->def().name);
            return;
        }
        const int node = from->node();
        const std::string pinName(from->def().name);
        const PinType type = from->def().type;
        const bool output = from->output();
        auto fits = [&](const PinDef& p) {
            return output ? canLink(static_cast<int>(type), static_cast<int>(p.type))
                          : canLink(static_cast<int>(p.type), static_cast<int>(type));
        };
        const QPoint global = view_->viewport()->mapToGlobal(view_->mapFromScene(scenePos));
        const std::string chosen = pickNode(this, global, library(), [&](const NodeDef& d) {
            return std::ranges::any_of(output ? d.inputs : d.outputs, fits);
        });
        if (chosen.empty()) return;
        const int added = addNode(chosen, scenePos);
        if (added == 0) return;
        GraphNodeItem* item = nodes_[added];
        if (!output) item->setPos(scenePos - QPointF(item->width(), kHeader));  // ends at the drop point
        else item->setPos(scenePos - QPointF(0, kHeader));
        for (const PinDef& p : output ? item->def().inputs : item->def().outputs) {
            if (!fits(p)) continue;
            output ? connectPins(node, pinName, added, p.name) : connectPins(added, p.name, node, pinName);
            break;
        }
        return;
    }
    GraphPinItem* out = from->output() ? from : target;
    GraphPinItem* in = from->output() ? target : from;
    connectPins(out->node(), out->def().name, in->node(), in->def().name);
}

void GraphEditorPanel::showNodePicker(QPointF scenePosition) {
    if (file_.isEmpty()) return;
    const QPoint global = view_->viewport()->mapToGlobal(view_->mapFromScene(scenePosition));
    const std::string type = pickNode(this, global, library());
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
    connect(typeBox, &QComboBox::currentTextChanged, this, [this] { changed(); });
    variables_->setCellWidget(row, 1, typeBox);
    variables_->setItem(row, 2, new QTableWidgetItem(type == QLatin1String("bool") ? QStringLiteral("false")
                                                     : type == QLatin1String("vec3") ? QStringLiteral("0, 0, 0")
                                                     : type == QLatin1String("string") ? QString()
                                                                                       : QStringLiteral("0")));
    loading_ = wasLoading;
    if (!loading_) changed();
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
    nlohmann::json comments = nlohmann::json::array();
    for (const GraphCommentItem* c : comments_) {
        comments.push_back({{"x", std::round(c->pos().x())},
                            {"y", std::round(c->pos().y())},
                            {"w", std::round(c->size().width())},
                            {"h", std::round(c->size().height())},
                            {"text", c->text().toStdString()},
                            {"color", c->color().name().toStdString()}});
    }
    nlohmann::json doc{{"version", 1}, {"variables", variablesJson()}, {"nodes", nodes}, {"links", links}};
    if (!comments.empty()) doc["comments"] = comments;
    return doc;
}

void GraphEditorPanel::loadDocument(const nlohmann::json& doc) {
    const bool wasLoading = loading_;
    loading_ = true;
    clear();
    loadVariables(doc.value("variables", nlohmann::json::array()));
    for (const auto& n : doc.value("nodes", nlohmann::json::array())) {
        if (!n.is_object()) continue;
        const NodeDef* def = findNode(n.value("type", std::string()));
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
    for (const auto& c : doc.value("comments", nlohmann::json::array())) {
        if (!c.is_object()) continue;
        auto* item = new GraphCommentItem(*this, QSizeF(c.value("w", 300.0), c.value("h", 200.0)),
                                          QString::fromStdString(c.value("text", std::string())),
                                          QColor(QString::fromStdString(c.value("color", std::string("#4f8cff")))));
        scene_->addItem(item);
        item->setPos(c.value("x", 0.0), c.value("y", 0.0));
        comments_.push_back(item);
    }
    refreshLiteralEditors();
    loading_ = wasLoading;
    live_ = false;  // the next refresh lights the new items
}

bool GraphEditorPanel::open(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const nlohmann::json doc = nlohmann::json::parse(f.readAll().toStdString(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        QMessageBox::warning(this, kind_ == GraphKind::Material ? tr("Material Graph") : tr("Visual Script"),
                             tr("%1 is not a valid graph.").arg(QFileInfo(path).fileName()));
        return false;
    }
    file_ = path;
    liveKey_ = visual::trace::keyOf(std::filesystem::path(path.toStdU16String()));
    loadDocument(doc);
    undo_.clear();
    redo_.clear();
    commitTimer_->stop();
    committed_ = document();
    saved_ = committed_;
    dirty_ = true;  // so setDirty(false) updates the title
    setDirty(false);
    QTimer::singleShot(0, this, [this] { fitGraph(); });
    materialChanged();
    return true;
}

bool GraphEditorPanel::save() {
    if (file_.isEmpty()) return false;
    QFile f(file_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, kind_ == GraphKind::Material ? tr("Material Graph") : tr("Visual Script"),
                             tr("Could not write %1.").arg(file_));
        return false;
    }
    if (commitTimer_->isActive()) commitUndo();
    saved_ = document();
    nlohmann::json written = saved_;
    if (kind_ == GraphKind::Material) written["kind"] = "material";
    const std::string text = written.dump(2) + "\n";
    f.write(text.data(), static_cast<qint64>(text.size()));
    f.close();
    setDirty(false);
    if (kind_ == GraphKind::Material) {
        emit materialSaved(file_);  // first: the scene (and the preview) re-read the file
        materialChanged();
    }
    return true;
}

} // namespace lumora::editor
