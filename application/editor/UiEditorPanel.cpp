#include "editor/UiEditorPanel.hpp"

#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <glm/common.hpp>

#include <algorithm>
#include <format>

namespace lumora::editor {

namespace {

constexpr float kHandle = 8.0f;  // screen pixels of the resize handle

QColor toColor(glm::vec4 c) {
    return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f),
                            std::clamp(c.a, 0.0f, 1.0f));
}

const char* typeLabel(UiWidget::Type t) {
    switch (t) {
        case UiWidget::Type::Panel: return "Panel";
        case UiWidget::Type::Label: return "Label";
        case UiWidget::Type::Button: return "Button";
        case UiWidget::Type::Checkbox: return "Checkbox";
        case UiWidget::Type::Slider: return "Slider";
        case UiWidget::Type::Image: return "Image";
    }
    return "Widget";
}

struct AnchorPreset {
    const char* label;
    glm::vec2 min, max;
};
constexpr AnchorPreset kPresets[] = {
    {"Top Left", {0, 0}, {0, 0}},       {"Top", {0.5f, 0}, {0.5f, 0}},       {"Top Right", {1, 0}, {1, 0}},
    {"Left", {0, 0.5f}, {0, 0.5f}},     {"Center", {0.5f, 0.5f}, {0.5f, 0.5f}}, {"Right", {1, 0.5f}, {1, 0.5f}},
    {"Bottom Left", {0, 1}, {0, 1}},    {"Bottom", {0.5f, 1}, {0.5f, 1}},    {"Bottom Right", {1, 1}, {1, 1}},
    {"Stretch Top", {0, 0}, {1, 0}},    {"Stretch Bottom", {0, 1}, {1, 1}},  {"Stretch", {0, 0}, {1, 1}},
};

QString parentPath(const QString& path) {
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

int indexOf(const QString& path) { return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1).toInt(); }

QString childPath(const QString& parent, int index) {
    return parent.isEmpty() ? QString::number(index) : parent + QLatin1Char('/') + QString::number(index);
}

} // namespace

// ---- Canvas -------------------------------------------------------------------------------------

class UiCanvas final : public QWidget {
public:
    explicit UiCanvas(UiEditorPanel& panel) : panel_(panel) {
        setMinimumSize(320, 200);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
    }

    /// Document pixels to widget pixels: scale and origin that fit the resolution.
    [[nodiscard]] std::pair<float, QPointF> transform() const {
        const glm::vec2 res = panel_.resolution();
        const float scale = std::min((static_cast<float>(width()) - 24.0f) / res.x, (static_cast<float>(height()) - 24.0f) / res.y);
        const QPointF origin((width() - res.x * scale) * 0.5, (height() - res.y * scale) * 0.5);
        return {std::max(scale, 0.01f), origin};
    }

    void draw(QPainter& p) {
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(0x1b, 0x1d, 0x22));
        const auto [scale, origin] = transform();
        const glm::vec2 res = panel_.resolution();
        auto toWidget = [&](glm::vec4 r) {
            return QRectF(origin.x() + r.x * scale, origin.y() + r.y * scale, r.z * scale, r.w * scale);
        };
        // The game's screen.
        const QRectF screen(origin, QSizeF(res.x * scale, res.y * scale));
        p.fillRect(screen, QColor(0x3a, 0x4a, 0x5c));
        p.setPen(QPen(QColor(0x5a, 0x6a, 0x7c), 1));
        p.drawRect(screen);
        for (const UiPlaced& placed : panel_.layout()) {
            const UiWidget& w = *placed.widget;
            const QRectF r = toWidget(placed.rect);
            p.save();
            if (placed.clip.x > -1.0e8f) {
                p.setClipRect(toWidget({placed.clip.x, placed.clip.y, placed.clip.z - placed.clip.x, placed.clip.w - placed.clip.y}));
            }
            QFont font = p.font();
            font.setPixelSize(std::max(1, static_cast<int>(w.fontSize * scale)));
            p.setFont(font);
            const QString text = QString::fromStdString(w.text);
            switch (w.type) {
                case UiWidget::Type::Panel:
                    if (w.color.a > 0.0f) p.fillRect(r, toColor(w.color));
                    p.setPen(QPen(QColor(255, 255, 255, 40), 1, Qt::DashLine));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(r);
                    break;
                case UiWidget::Type::Label: {
                    p.setPen(toColor(w.color));
                    const Qt::Alignment align = w.align == 1 ? Qt::AlignHCenter : w.align == 2 ? Qt::AlignRight : Qt::AlignLeft;
                    p.drawText(r, align | Qt::AlignVCenter, text);
                    break;
                }
                case UiWidget::Type::Button:
                    p.fillRect(r, QColor::fromRgbF(0.16f, 0.17f, 0.21f, 0.92f));
                    p.setPen(QColor(242, 245, 250));
                    p.drawText(r, Qt::AlignCenter, text);
                    break;
                case UiWidget::Type::Checkbox: {
                    const qreal box = std::min(r.height() - 6.0 * scale, 26.0 * scale);
                    const QRectF b(r.left() + 4.0 * scale, r.center().y() - box / 2, box, box);
                    p.fillRect(b, QColor::fromRgbF(0.16f, 0.17f, 0.21f, 0.92f));
                    if (w.value != 0.0f) p.fillRect(b.adjusted(5 * scale, 5 * scale, -5 * scale, -5 * scale), QColor(84, 143, 242));
                    p.setPen(QColor(242, 245, 250));
                    p.drawText(r.adjusted(box + 8 * scale, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
                    break;
                }
                case UiWidget::Type::Slider: {
                    p.fillRect(r, QColor::fromRgbF(0.11f, 0.12f, 0.15f, 0.95f));
                    const float t = w.max > w.min ? std::clamp((w.value - w.min) / (w.max - w.min), 0.0f, 1.0f) : 0.0f;
                    p.fillRect(QRectF(r.left(), r.top(), r.width() * t, r.height()), QColor(63, 107, 181));
                    p.setPen(QColor(242, 245, 250));
                    p.drawText(r.adjusted(10 * scale, 0, -10 * scale, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
                    p.drawText(r.adjusted(10 * scale, 0, -10 * scale, 0), Qt::AlignRight | Qt::AlignVCenter,
                               QString::number(w.value, 'f', 2));
                    break;
                }
                case UiWidget::Type::Image: {
                    const QImage picture = panel_.image(w.image);
                    if (!picture.isNull()) {
                        p.drawImage(r, picture);
                    } else {
                        p.fillRect(r, QColor(255, 255, 255, 30));
                        p.setPen(QColor(255, 255, 255, 120));
                        p.drawText(r, Qt::AlignCenter, QStringLiteral("image"));
                    }
                    break;
                }
            }
            p.restore();
            if (QString::fromStdString(placed.path) == panel_.selected()) {
                p.setPen(QPen(QColor(255, 170, 40), 2));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r);
                p.fillRect(QRectF(r.right() - kHandle / 2, r.bottom() - kHandle / 2, kHandle, kHandle), QColor(255, 170, 40));
            }
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        draw(p);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const auto [scale, origin] = transform();
        const QPointF at = e->position();
        last_ = at;
        mode_ = Mode::None;
        // The selected widget's corner handle resizes it.
        const std::vector<UiPlaced> placed = panel_.layout();
        for (const UiPlaced& p : placed) {
            if (QString::fromStdString(p.path) != panel_.selected() || p.path.empty()) continue;
            const QPointF corner(origin.x() + (p.rect.x + p.rect.z) * scale, origin.y() + (p.rect.y + p.rect.w) * scale);
            if (std::abs(at.x() - corner.x()) <= kHandle && std::abs(at.y() - corner.y()) <= kHandle) mode_ = Mode::Resize;
        }
        if (mode_ == Mode::None) {
            // The topmost widget under the cursor (drawn last), else the root.
            QString hit;
            for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
                const glm::vec2 d((at.x() - origin.x()) / scale, (at.y() - origin.y()) / scale);
                const glm::vec4 r = it->rect;
                if (d.x >= r.x && d.y >= r.y && d.x < r.x + r.z && d.y < r.y + r.w) {
                    hit = QString::fromStdString(it->path);
                    break;
                }
            }
            panel_.select(hit);
            if (!hit.isEmpty()) mode_ = Mode::Move;
        }
        moved_ = false;
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (mode_ == Mode::None || !(e->buttons() & Qt::LeftButton)) return;
        const auto [scale, origin] = transform();
        const QPointF delta = (e->position() - last_) / scale;
        last_ = e->position();
        const glm::vec2 d(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        if (mode_ == Mode::Move) {
            panel_.moveWidget(panel_.selected(), d, false);
        } else {
            panel_.resizeWidget(panel_.selected(), d, false);
        }
        moved_ = true;
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        if (moved_) panel_.changed();  // the whole drag is one undo step
        mode_ = Mode::None;
        moved_ = false;
    }

private:
    enum class Mode { None, Move, Resize };
    UiEditorPanel& panel_;
    Mode mode_ = Mode::None;
    QPointF last_;
    bool moved_ = false;
};

// ---- Panel ----------------------------------------------------------------------------------------

UiEditorPanel::UiEditorPanel(QString projectRoot, AssetPicker pickAsset, QWidget* parent)
    : QWidget(parent), root_(std::move(projectRoot)), pickAsset_(std::move(pickAsset)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    title_ = new QLabel(tr("No UI document open — create one in the Content Browser (New > UI Document)"));
    bar->addWidget(title_, 1);
    auto button = [&](const QString& icon, const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        if (!icon.isEmpty()) b->setIcon(app::theme::icon(icon));
        if (!text.isEmpty()) {
            b->setText(text);
            b->setToolButtonStyle(icon.isEmpty() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
        }
        b->setToolTip(tip);
        bar->addWidget(b);
        return b;
    };
    auto* add = button(QStringLiteral("plus"), tr("Add"), tr("Add a widget into the selected panel"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add);
    for (UiWidget::Type t : {UiWidget::Type::Panel, UiWidget::Type::Label, UiWidget::Type::Button, UiWidget::Type::Checkbox,
                             UiWidget::Type::Slider, UiWidget::Type::Image}) {
        addMenu->addAction(tr(typeLabel(t)), this, [this, t] { addWidget(t); });
    }
    add->setMenu(addMenu);
    auto* remove = button(QStringLiteral("trash"), {}, tr("Remove the selected widget (Delete)"));
    auto* up = button({}, QStringLiteral("↑"), tr("Move up among its siblings"));
    auto* down = button({}, QStringLiteral("↓"), tr("Move down among its siblings"));
    auto* undoButton = button(QStringLiteral("undo"), {}, tr("Undo (Ctrl+Z)"));
    auto* redoButton = button(QStringLiteral("redo"), {}, tr("Redo (Ctrl+Shift+Z)"));
    resolutionBox_ = new QComboBox;
    resolutionBox_->setObjectName(QStringLiteral("uiResolution"));
    for (const auto& [label, w, h] : {std::tuple{"1280 × 720", 1280, 720}, std::tuple{"1920 × 1080", 1920, 1080},
                                      std::tuple{"800 × 600", 800, 600}, std::tuple{"720 × 1280", 720, 1280}}) {
        resolutionBox_->addItem(QString::fromUtf8(label), QSize(w, h));
    }
    resolutionBox_->setToolTip(tr("The screen the layout is shown on (anchors keep it right on others)"));
    bar->addWidget(resolutionBox_);
    auto* saveButton = button(QStringLiteral("save"), tr("Save"), tr("Save (Ctrl+S)"));
    layout->addLayout(bar);

    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setMinimumWidth(170);
    canvas_ = new UiCanvas(*this);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    properties_ = new QWidget;
    form_ = new QFormLayout(properties_);
    form_->setContentsMargins(6, 6, 6, 6);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(properties_);
    scroll->setMinimumWidth(260);
    auto* split = new QSplitter;
    split->addWidget(tree_);
    split->addWidget(canvas_);
    split->addWidget(scroll);
    split->setStretchFactor(1, 1);
    split->setSizes({190, 700, 280});
    layout->addWidget(split, 1);

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!rebuilding_ && item != nullptr) select(item->data(0, Qt::UserRole).toString());
    });
    connect(remove, &QToolButton::clicked, this, [this] { removeWidget(selected_); });
    connect(up, &QToolButton::clicked, this, [this] { select(moveInParent(selected_, -1)); });
    connect(down, &QToolButton::clicked, this, [this] { select(moveInParent(selected_, 1)); });
    connect(undoButton, &QToolButton::clicked, this, &UiEditorPanel::undo);
    connect(redoButton, &QToolButton::clicked, this, &UiEditorPanel::redo);
    connect(saveButton, &QToolButton::clicked, this, &UiEditorPanel::save);
    connect(resolutionBox_, &QComboBox::currentIndexChanged, this, [this] {
        const QSize s = resolutionBox_->currentData().toSize();
        setResolution({static_cast<float>(s.width()), static_cast<float>(s.height())});
    });
    auto shortcut = [this](const QKeySequence& keys, auto fn) {
        auto* s = new QShortcut(keys, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    shortcut(QKeySequence::Save, [this] { save(); });
    shortcut(QKeySequence::Undo, [this] { undo(); });
    shortcut(QKeySequence::Redo, [this] { redo(); });
    shortcut(QKeySequence(Qt::Key_Delete), [this] { removeWidget(selected_); });
    setEnabled(true);
}

UiEditorPanel::~UiEditorPanel() = default;

bool UiEditorPanel::open(const QString& file) {
    auto loaded = loadUiDocument(app::toPath(file));
    if (!loaded) {
        QMessageBox::warning(this, tr("UI Editor"), QString::fromStdString(loaded.error().message));
        return false;
    }
    file_ = file;
    document_ = std::move(*loaded);
    undo_.clear();
    redo_.clear();
    committed_ = uiToJson(document_);
    selected_.clear();
    images_.clear();
    dirty_ = true;
    setDirty(false);
    rebuildTree();
    rebuildProperties();
    canvas_->update();
    return true;
}

bool UiEditorPanel::save() {
    if (file_.isEmpty()) return false;
    if (auto written = saveUiDocument(document_, app::toPath(file_)); !written) {
        QMessageBox::warning(this, tr("UI Editor"), QString::fromStdString(written.error().message));
        return false;
    }
    setDirty(false);
    emit saved(file_);
    return true;
}

void UiEditorPanel::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    if (!file_.isEmpty()) title_->setText(QFileInfo(file_).fileName() + (dirty ? QStringLiteral(" •") : QString()));
    emit dirtyChanged(dirty);
}

UiWidget* UiEditorPanel::widgetAt(const QString& path) { return uiWidgetAt(document_.root, path.toStdString()); }

std::vector<UiPlaced> UiEditorPanel::layout() const { return layoutUi(document_, resolution_); }

void UiEditorPanel::changed() {
    nlohmann::json now = uiToJson(document_);
    if (now == committed_) return;
    undo_.push_back(std::move(committed_));
    committed_ = std::move(now);
    redo_.clear();
    setDirty(true);
    refreshViews();
}

void UiEditorPanel::refreshViews() {
    rebuildTree();
    for (const Refresher& r : refreshers_) r();
    canvas_->update();
}

void UiEditorPanel::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(committed_));
    committed_ = std::move(undo_.back());
    undo_.pop_back();
    if (auto doc = uiFromJson(committed_)) document_ = std::move(*doc);
    if (widgetAt(selected_) == nullptr) selected_.clear();
    setDirty(true);
    rebuildTree();
    rebuildProperties();
    canvas_->update();
}

void UiEditorPanel::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(committed_));
    committed_ = std::move(redo_.back());
    redo_.pop_back();
    if (auto doc = uiFromJson(committed_)) document_ = std::move(*doc);
    if (widgetAt(selected_) == nullptr) selected_.clear();
    setDirty(true);
    rebuildTree();
    rebuildProperties();
    canvas_->update();
}

QString UiEditorPanel::panelFor(const QString& path) const {
    const UiWidget* w = uiWidgetAt(const_cast<UiWidget&>(document_.root), path.toStdString());
    if (w != nullptr && w->type == UiWidget::Type::Panel) return path;
    return w != nullptr ? parentPath(path) : QString();
}

QString UiEditorPanel::addWidget(UiWidget::Type type, const QString& parent) {
    if (file_.isEmpty()) return {};
    const QString into = panelFor(parent.isNull() ? selected_ : parent);
    UiWidget* panel = widgetAt(into);
    if (panel == nullptr || panel->type != UiWidget::Type::Panel) return {};
    UiWidget w;
    w.type = type;
    // A name not used yet: "Button", "Button2", ...
    std::string name = typeLabel(type);
    for (int n = 2; findUiWidget(document_.root, name) != nullptr; ++n) name = std::format("{}{}", typeLabel(type), n);
    w.name = name;
    switch (type) {
        case UiWidget::Type::Panel:
            w.color = {0.06f, 0.07f, 0.09f, 0.82f};
            w.offsetMax = {300.0f, 200.0f};
            w.size = {0.0f, 120.0f};
            w.padding = glm::vec4(10.0f);
            break;
        case UiWidget::Type::Label: w.text = "Label"; break;
        case UiWidget::Type::Button:
            w.text = "Button";
            w.offsetMax = {200.0f, 48.0f};
            w.size = {0.0f, 48.0f};
            break;
        case UiWidget::Type::Checkbox: w.text = "Option"; break;
        case UiWidget::Type::Slider:
            w.text = "Value";
            w.offsetMax = {260.0f, 40.0f};
            w.value = 0.5f;
            break;
        case UiWidget::Type::Image:
            w.offsetMax = {64.0f, 64.0f};
            w.size = {64.0f, 64.0f};
            break;
    }
    panel->children.push_back(std::move(w));
    const QString path = childPath(into, static_cast<int>(panel->children.size()) - 1);
    selected_ = path;
    changed();
    rebuildProperties();
    return path;
}

bool UiEditorPanel::removeWidget(const QString& path) {
    if (path.isEmpty()) return false;  // the root stays
    UiWidget* parent = widgetAt(parentPath(path));
    const int index = indexOf(path);
    if (parent == nullptr || index < 0 || index >= static_cast<int>(parent->children.size())) return false;
    parent->children.erase(parent->children.begin() + index);
    selected_.clear();
    changed();
    rebuildProperties();
    return true;
}

QString UiEditorPanel::moveInParent(const QString& path, int delta) {
    if (path.isEmpty()) return path;
    UiWidget* parent = widgetAt(parentPath(path));
    const int index = indexOf(path);
    const int target = index + delta;
    if (parent == nullptr || target < 0 || target >= static_cast<int>(parent->children.size())) return path;
    std::swap(parent->children[static_cast<std::size_t>(index)], parent->children[static_cast<std::size_t>(target)]);
    const QString moved = childPath(parentPath(path), target);
    selected_ = moved;
    changed();
    return moved;
}

void UiEditorPanel::select(const QString& path) {
    if (path == selected_ && !path.isNull()) return;
    selected_ = widgetAt(path) != nullptr ? path : QString();
    rebuildTree();
    rebuildProperties();
    canvas_->update();
}

void UiEditorPanel::moveWidget(const QString& path, glm::vec2 delta, bool commit) {
    UiWidget* w = widgetAt(path);
    const UiWidget* parent = widgetAt(parentPath(path));
    if (w == nullptr || path.isEmpty() || parent == nullptr || parent->layout != UiWidget::Layout::None) return;
    w->offsetMin += delta;
    w->offsetMax += delta;
    if (commit) {
        changed();
    } else {
        canvas_->update();
        for (const Refresher& r : refreshers_) r();
    }
}

void UiEditorPanel::moveWidget(const QString& path, glm::vec2 delta) { moveWidget(path, delta, true); }

void UiEditorPanel::resizeWidget(const QString& path, glm::vec2 delta, bool commit) {
    UiWidget* w = widgetAt(path);
    const UiWidget* parent = widgetAt(parentPath(path));
    if (w == nullptr || path.isEmpty() || parent == nullptr) return;
    if (parent->layout == UiWidget::Layout::None) {
        w->offsetMax += delta;
    } else {
        // In stacks and grids the size is what the panel lays out.
        if (parent->layout == UiWidget::Layout::Horizontal) w->size.x = std::max(w->size.x + delta.x, 1.0f);
        w->size.y = std::max(w->size.y + delta.y, 1.0f);
    }
    if (commit) {
        changed();
    } else {
        canvas_->update();
        for (const Refresher& r : refreshers_) r();
    }
}

void UiEditorPanel::resizeWidget(const QString& path, glm::vec2 delta) { resizeWidget(path, delta, true); }

void UiEditorPanel::setAnchors(const QString& path, glm::vec2 anchorMin, glm::vec2 anchorMax) {
    UiWidget* w = widgetAt(path);
    if (w == nullptr) return;
    // Where it is now, and the area its anchors refer to (the parent's content, or the screen).
    glm::vec4 rect{0.0f}, content{0.0f, 0.0f, resolution_.x, resolution_.y};
    const std::string self = path.toStdString(), parent = parentPath(path).toStdString();
    for (const UiPlaced& p : layout()) {
        if (p.path == self) rect = p.rect;
        if (!path.isEmpty() && p.path == parent) {
            const glm::vec4 pad = p.widget->padding;
            content = {p.rect.x + pad.x, p.rect.y + pad.y, p.rect.z - pad.x - pad.z, p.rect.w - pad.y - pad.w};
        }
    }
    const glm::vec2 origin(content.x, content.y), size(content.z, content.w);
    w->anchorMin = anchorMin;
    w->anchorMax = anchorMax;
    w->offsetMin = glm::vec2(rect.x, rect.y) - (origin + anchorMin * size);
    w->offsetMax = glm::vec2(rect.x + rect.z, rect.y + rect.w) - (origin + anchorMax * size);
    changed();
}

void UiEditorPanel::editSelected(const std::function<void(UiWidget&)>& edit) {
    if (UiWidget* w = widgetAt(selected_)) {
        edit(*w);
        changed();
    }
}

void UiEditorPanel::setResolution(glm::vec2 size) {
    resolution_ = glm::max(size, glm::vec2(16.0f));
    canvas_->update();
}

QImage UiEditorPanel::canvasImage() const {
    QImage image(canvas_->size().isEmpty() ? QSize(640, 360) : canvas_->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter p(&image);
    canvas_->draw(p);
    return image;
}

QImage UiEditorPanel::image(const std::string& key) {
    if (key.empty() || key.find('#') != std::string::npos) return {};
    const auto it = images_.find(key);
    if (it != images_.end()) return it->second;
    QImage loaded(QDir(root_).filePath(QString::fromStdString(key)));
    return images_[key] = loaded;
}

void UiEditorPanel::rebuildTree() {
    rebuilding_ = true;
    tree_->clear();
    QTreeWidgetItem* current = nullptr;
    auto add = [&](auto& self, const UiWidget& w, const QString& path, QTreeWidgetItem* parent) -> void {
        auto* item = parent == nullptr ? new QTreeWidgetItem(tree_) : new QTreeWidgetItem(parent);
        item->setText(0, QStringLiteral("%1  %2").arg(tr(typeLabel(w.type)), QString::fromStdString(w.name)));
        item->setData(0, Qt::UserRole, path);
        if (!w.visible) item->setForeground(0, QColor(130, 130, 130));
        if (path == selected_) current = item;
        for (std::size_t i = 0; i < w.children.size(); ++i) self(self, w.children[i], childPath(path, static_cast<int>(i)), item);
    };
    if (!file_.isEmpty()) add(add, document_.root, QString(), nullptr);
    tree_->expandAll();
    if (current != nullptr) tree_->setCurrentItem(current);
    rebuilding_ = false;
}

void UiEditorPanel::rebuildProperties() {
    refreshers_.clear();
    while (form_->rowCount() > 0) form_->removeRow(0);
    fieldDescs_.clear();
    UiWidget* selected = widgetAt(selected_);
    if (file_.isEmpty() || selected == nullptr) {
        auto* hint = new QLabel(tr("Select a widget on the canvas or in the list."));
        hint->setWordWrap(true);
        hint->setProperty("muted", true);
        form_->addRow(hint);
        return;
    }
    const UiWidget::Type type = selected->type;
    const UiWidget* parent = selected_.isEmpty() ? nullptr : widgetAt(parentPath(selected_));
    const bool anchored = parent == nullptr || parent->layout == UiWidget::Layout::None;
    auto heading = [&](const QString& text) {
        auto* label = new QLabel(text);
        label->setStyleSheet(QStringLiteral("font-weight: 600; padding-top: 6px;"));
        form_->addRow(label);
    };
    // One field: a description and accessors on the selected widget.
    auto field = [&](const char* label, FieldKind kind, std::function<FieldValue(const UiWidget&)> get,
                     std::function<void(UiWidget&, const FieldValue&)> set, float min = -1.0e6f, float max = 1.0e6f, float step = 1.0f,
                     std::vector<std::string> labels = {}) {
        auto desc = std::make_unique<FieldDesc>();
        desc->id = label;
        desc->label = label;
        desc->kind = kind;
        desc->min = min;
        desc->max = max;
        desc->step = step;
        desc->enumLabels = std::move(labels);
        FieldGetter getter = [this, get] {
            const UiWidget* w = widgetAt(selected_);
            return w != nullptr ? get(*w) : FieldValue(0.0f);
        };
        FieldSetter setter = [this, set](const FieldValue& v) { editSelected([&](UiWidget& w) { set(w, v); }); };
        QWidget* editor = createFieldEditor(*desc, std::move(getter), std::move(setter), refreshers_, pickAsset_);
        auto* l = new QLabel(tr(label));
        l->setProperty("muted", true);
        form_->addRow(l, editor);
        fieldDescs_.push_back(std::move(desc));
    };
    auto number = [&](const char* label, float UiWidget::*member, float min = -1.0e6f, float max = 1.0e6f, float step = 1.0f) {
        field(label, FieldKind::Float, [member](const UiWidget& w) { return FieldValue(w.*member); },
              [member](UiWidget& w, const FieldValue& v) { w.*member = std::get<float>(v); }, min, max, step);
    };
    auto component = [&](const char* label, auto member, int i, float min = -1.0e6f, float max = 1.0e6f, float step = 1.0f) {
        field(label, FieldKind::Float, [member, i](const UiWidget& w) { return FieldValue((w.*member)[i]); },
              [member, i](UiWidget& w, const FieldValue& v) { (w.*member)[i] = std::get<float>(v); }, min, max, step);
    };
    auto text = [&](const char* label, std::string UiWidget::*member, FieldKind kind = FieldKind::String) {
        field(label, kind, [member](const UiWidget& w) { return FieldValue(w.*member); },
              [member](UiWidget& w, const FieldValue& v) { w.*member = std::get<std::string>(v); });
    };
    auto flag = [&](const char* label, bool UiWidget::*member) {
        field(label, FieldKind::Bool, [member](const UiWidget& w) { return FieldValue(w.*member); },
              [member](UiWidget& w, const FieldValue& v) { w.*member = std::get<bool>(v); });
    };

    heading(tr("%1").arg(tr(typeLabel(type))));
    text("Name", &UiWidget::name);
    flag("Visible", &UiWidget::visible);
    if (anchored && !selected_.isEmpty()) {
        heading(tr("Placement"));
        // Presets set the anchors keeping the rectangle where it is.
        auto* anchor = new QComboBox;
        anchor->setObjectName(QStringLiteral("uiAnchorPreset"));
        anchor->addItem(tr("Custom"));
        for (const AnchorPreset& p : kPresets) anchor->addItem(tr(p.label));
        auto current = [this] {
            const UiWidget* w = widgetAt(selected_);
            if (w == nullptr) return 0;
            for (std::size_t i = 0; i < std::size(kPresets); ++i) {
                if (w->anchorMin == kPresets[i].min && w->anchorMax == kPresets[i].max) return static_cast<int>(i) + 1;
            }
            return 0;
        };
        anchor->setCurrentIndex(current());
        refreshers_.push_back([anchor, current] {
            const QSignalBlocker block(anchor);
            anchor->setCurrentIndex(current());
        });
        const QString path = selected_;
        connect(anchor, &QComboBox::activated, this, [this, path](int index) {
            if (index <= 0 || index > static_cast<int>(std::size(kPresets))) return;
            const AnchorPreset& p = kPresets[static_cast<std::size_t>(index - 1)];
            setAnchors(path, p.min, p.max);
        });
        auto* anchorLabel = new QLabel(tr("Anchor"));
        anchorLabel->setProperty("muted", true);
        form_->addRow(anchorLabel, anchor);
        component("Anchor Min X", &UiWidget::anchorMin, 0, 0.0f, 1.0f, 0.05f);
        component("Anchor Min Y", &UiWidget::anchorMin, 1, 0.0f, 1.0f, 0.05f);
        component("Anchor Max X", &UiWidget::anchorMax, 0, 0.0f, 1.0f, 0.05f);
        component("Anchor Max Y", &UiWidget::anchorMax, 1, 0.0f, 1.0f, 0.05f);
        component("Left", &UiWidget::offsetMin, 0);
        component("Top", &UiWidget::offsetMin, 1);
        component("Right", &UiWidget::offsetMax, 0);
        component("Bottom", &UiWidget::offsetMax, 1);
    } else if (!selected_.isEmpty()) {
        heading(tr("Size in the panel (0 = full)"));
        component("Width", &UiWidget::size, 0, 0.0f);
        component("Height", &UiWidget::size, 1, 0.0f);
    }
    heading(tr("Look"));
    field("Color", FieldKind::Color, [](const UiWidget& w) { return FieldValue(glm::vec3(w.color)); },
          [](UiWidget& w, const FieldValue& v) {
              const glm::vec3 c = std::get<glm::vec3>(v);
              w.color = glm::vec4(c, w.color.a);
          });
    component("Opacity", &UiWidget::color, 3, 0.0f, 1.0f, 0.05f);
    if (type != UiWidget::Type::Panel && type != UiWidget::Type::Image) {
        text("Text (@key: translated)", &UiWidget::text);
        number("Font Size", &UiWidget::fontSize, 4.0f, 400.0f);
        text("Font (.ttf / .otf)", &UiWidget::font, FieldKind::Asset);
    }
    if (type == UiWidget::Type::Label) {
        field("Align", FieldKind::Enum, [](const UiWidget& w) { return FieldValue(w.align); },
              [](UiWidget& w, const FieldValue& v) { w.align = std::get<int>(v); }, 0, 0, 0, {"Left", "Center", "Right"});
    }
    if (type == UiWidget::Type::Image) text("Picture", &UiWidget::image, FieldKind::Asset);
    if (type == UiWidget::Type::Checkbox) {
        field("Checked", FieldKind::Bool, [](const UiWidget& w) { return FieldValue(w.value != 0.0f); },
              [](UiWidget& w, const FieldValue& v) { w.value = std::get<bool>(v) ? 1.0f : 0.0f; });
    }
    if (type == UiWidget::Type::Slider) {
        number("Value", &UiWidget::value, -1.0e6f, 1.0e6f, 0.05f);
        number("Min", &UiWidget::min, -1.0e6f, 1.0e6f, 0.05f);
        number("Max", &UiWidget::max, -1.0e6f, 1.0e6f, 0.05f);
    }
    if (type == UiWidget::Type::Panel) {
        heading(tr("Children"));
        field("Layout", FieldKind::Enum, [](const UiWidget& w) { return FieldValue(static_cast<int>(w.layout)); },
              [](UiWidget& w, const FieldValue& v) { w.layout = static_cast<UiWidget::Layout>(std::get<int>(v)); }, 0, 0, 0,
              {"Free (anchors)", "Column", "Row", "Grid"});
        field("Columns", FieldKind::Int, [](const UiWidget& w) { return FieldValue(w.columns); },
              [](UiWidget& w, const FieldValue& v) { w.columns = std::max(std::get<int>(v), 1); }, 1, 32, 1);
        number("Spacing", &UiWidget::spacing, 0.0f, 1000.0f);
        component("Padding Left", &UiWidget::padding, 0, 0.0f);
        component("Padding Top", &UiWidget::padding, 1, 0.0f);
        component("Padding Right", &UiWidget::padding, 2, 0.0f);
        component("Padding Bottom", &UiWidget::padding, 3, 0.0f);
        flag("Scroll", &UiWidget::scroll);
    }
}

} // namespace lumora::editor
