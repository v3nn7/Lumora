#include "editor/PropertyWidgets.hpp"

#include "common/Theme.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QSlider>

#include <array>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lumora::editor {

namespace {

QColor toQColor(const glm::vec3& c) {
    return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f));
}

glm::vec3 fromQColor(const QColor& c) {
    return {static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF())};
}

int decimalsFor(float step) {
    if (step >= 1.0f) return 1;
    return std::clamp(static_cast<int>(std::ceil(-std::log10(step))) + 1, 2, 4);
}

QDoubleSpinBox* makeSpin(const FieldDesc& d) {
    auto* spin = new QDoubleSpinBox;
    const double min = d.min <= std::numeric_limits<float>::lowest() / 2 ? -1e9 : d.min;
    const double max = d.max >= std::numeric_limits<float>::max() / 2 ? 1e9 : d.max;
    spin->setRange(min, max);
    spin->setSingleStep(d.step);
    spin->setDecimals(decimalsFor(d.step));
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setKeyboardTracking(false);
    spin->setAccelerated(true);
    spin->setMinimumWidth(48);
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return spin;
}

bool editing(const QWidget* w) { return w->hasFocus() || w->isAncestorOf(QApplication::focusWidget()); }

} // namespace

ScrubLabel::ScrubLabel(const QString& text, QDoubleSpinBox* target, QWidget* parent)
    : QLabel(text, parent), target_(target) {
    setCursor(Qt::SizeHorCursor);
    setToolTip(tr("Drag to adjust (Shift: fine)"));
}

void ScrubLabel::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        last_ = e->globalPosition();
    }
}

void ScrubLabel::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    const double dx = e->globalPosition().x() - last_.x();
    last_ = e->globalPosition();
    const double scale = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 1.0;
    target_->setValue(target_->value() + dx * target_->singleStep() * scale);
}

void ScrubLabel::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

ColorButton::ColorButton(QWidget* parent) : QPushButton(parent) {
    setFixedHeight(24);
    setMinimumWidth(60);
    connect(this, &QPushButton::clicked, this, &ColorButton::pick);
}

void ColorButton::setColor(const QColor& c) {
    color_ = c;
    update();
}

void ColorButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(r, 5, 5);
    p.fillPath(path, color_);
    p.setPen(QPen(underMouse() ? QColor(0x5b, 0x95, 0xff) : QColor(0x3a, 0x40, 0x4a), 1));
    p.drawPath(path);
    p.setPen(color_.lightnessF() > 0.6 ? Qt::black : Qt::white);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() - 1.0);
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, color_.name(QColor::HexRgb).toUpper());
}

void ColorButton::pick() {
    const QColor original = color_;
    QColorDialog dialog(color_, window());
    dialog.setOption(QColorDialog::DontUseNativeDialog);
    connect(&dialog, &QColorDialog::currentColorChanged, this, [this](const QColor& c) {
        setColor(c);
        emit colorEdited(c);
    });
    if (dialog.exec() != QDialog::Accepted) {
        setColor(original);
        emit colorEdited(original);
    }
}

Section::Section(const QString& title, const QIcon& icon, bool removable, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 8);
    layout->setSpacing(0);

    auto* headerRow = new QWidget;
    headerRow->setObjectName(QStringLiteral("sectionHeader"));
    headerRow->setAttribute(Qt::WA_StyledBackground);
    headerRow->setStyleSheet(QStringLiteral(
        "#sectionHeader { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #22262e, stop:1 #1e2128);"
        "  border: 1px solid #2a2e37; border-radius: 7px; }"));
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(6, 3, 6, 3);
    headerLayout->setSpacing(4);
    header_ = new QToolButton;
    header_->setObjectName(QStringLiteral("sectionTitle"));
    header_->setText(title);
    header_->setIcon(icon);
    header_->setIconSize(QSize(15, 15));
    header_->setCheckable(true);
    header_->setChecked(true);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setStyleSheet(QStringLiteral("QToolButton { font-weight: 650; color: #e6e9ee; border: none; background: transparent;"
                                          "  padding: 3px 2px; } QToolButton:checked { background: transparent; }"));
    header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    headerLayout->addWidget(header_);
    if (removable) {
        auto* remove = new QToolButton;
        remove->setIcon(app::theme::icon(QStringLiteral("trash")));
        remove->setToolTip(tr("Remove component"));
        remove->setIconSize(QSize(13, 13));
        headerLayout->addWidget(remove);
        connect(remove, &QToolButton::clicked, this, &Section::removeRequested);
    }
    chevron_ = new QLabel(QStringLiteral("▾"));
    chevron_->setStyleSheet(QStringLiteral("color: #7c8494; padding: 0 2px;"));
    headerLayout->addWidget(chevron_);
    layout->addWidget(headerRow);

    body_ = new QWidget;
    layout->addWidget(body_);
    connect(header_, &QToolButton::toggled, this, [this](bool open) {
        body_->setVisible(open);
        chevron_->setText(open ? QStringLiteral("▾") : QStringLiteral("▸"));
    });
}

QWidget* createFieldEditor(const FieldDesc& d, FieldGetter get, FieldSetter set, std::vector<Refresher>& refreshers) {
    switch (d.kind) {
        case FieldKind::Bool: {
            auto* box = new QCheckBox;
            box->setChecked(std::get<bool>(get()));
            QObject::connect(box, &QCheckBox::toggled, box, [set](bool v) { set(FieldValue(v)); });
            refreshers.push_back([box, get] {
                const QSignalBlocker block(box);
                box->setChecked(std::get<bool>(get()));
            });
            return box;
        }
        case FieldKind::Enum: {
            auto* combo = new QComboBox;
            for (const auto& label : d.enumLabels) combo->addItem(QString::fromStdString(label));
            combo->setCurrentIndex(std::get<int>(get()));
            QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [set](int i) { set(FieldValue(i)); });
            refreshers.push_back([combo, get] {
                if (combo->view()->isVisible()) return;
                const QSignalBlocker block(combo);
                combo->setCurrentIndex(std::get<int>(get()));
            });
            return combo;
        }
        case FieldKind::Int:
        case FieldKind::Float: {
            auto* spin = makeSpin(d);
            const bool isInt = d.kind == FieldKind::Int;
            if (isInt) spin->setDecimals(0);
            auto read = [get, isInt] {
                const FieldValue v = get();
                return isInt ? static_cast<double>(std::get<int>(v)) : static_cast<double>(std::get<float>(v));
            };
            spin->setValue(read());
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [set, isInt](double v) {
                set(isInt ? FieldValue(static_cast<int>(std::lround(v))) : FieldValue(static_cast<float>(v)));
            });
            refreshers.push_back([spin, read] {
                if (editing(spin)) return;
                const QSignalBlocker block(spin);
                spin->setValue(read());
            });
            // A slider complements bounded 0..1-style ranges (metallic, roughness).
            if (!isInt && d.min >= 0.0f && d.max <= 1.0f) {
                auto* row = new QWidget;
                auto* layout = new QHBoxLayout(row);
                layout->setContentsMargins(0, 0, 0, 0);
                auto* slider = new QSlider(Qt::Horizontal);
                slider->setRange(0, 1000);
                auto toSlider = [d](double v) { return static_cast<int>((v - d.min) / (d.max - d.min) * 1000.0); };
                slider->setValue(toSlider(spin->value()));
                QObject::connect(slider, &QSlider::valueChanged, spin,
                                 [spin, d](int v) { spin->setValue(d.min + (d.max - d.min) * v / 1000.0); });
                QObject::connect(spin, &QDoubleSpinBox::valueChanged, slider, [slider, toSlider](double v) {
                    const QSignalBlocker block(slider);
                    slider->setValue(toSlider(v));
                });
                refreshers.push_back([slider, spin, toSlider] {
                    if (slider->isSliderDown()) return;
                    const QSignalBlocker block(slider);
                    slider->setValue(toSlider(spin->value()));
                });
                spin->setMaximumWidth(64);
                layout->addWidget(slider, 1);
                layout->addWidget(spin);
                return row;
            }
            return spin;
        }
        case FieldKind::Vec3: {
            auto* row = new QWidget;
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            std::array<QDoubleSpinBox*, 3> spins{};
            const char* axes[3] = {"X", "Y", "Z"};
            const char* colors[3] = {"#d9534f", "#5cb85c", "#4f8cff"};
            const glm::vec3 initial = std::get<glm::vec3>(get());
            for (int i = 0; i < 3; ++i) {
                spins[i] = makeSpin(d);
                spins[i]->setValue(initial[i]);
                auto* label = new ScrubLabel(QString::fromLatin1(axes[i]), spins[i]);
                label->setFixedWidth(16);
                label->setAlignment(Qt::AlignCenter);
                label->setStyleSheet(QStringLiteral("background: %1; color: white; border-radius: 3px; font-weight: 600;")
                                         .arg(QString::fromLatin1(colors[i])));
                layout->addWidget(label);
                layout->addWidget(spins[i], 1);
            }
            for (int i = 0; i < 3; ++i) {
                QObject::connect(spins[i], &QDoubleSpinBox::valueChanged, row, [spins, set](double) {
                    set(FieldValue(glm::vec3(static_cast<float>(spins[0]->value()), static_cast<float>(spins[1]->value()),
                                             static_cast<float>(spins[2]->value()))));
                });
            }
            refreshers.push_back([spins, get] {
                const glm::vec3 v = std::get<glm::vec3>(get());
                for (int i = 0; i < 3; ++i) {
                    if (editing(spins[i])) continue;
                    const QSignalBlocker block(spins[i]);
                    spins[i]->setValue(v[i]);
                }
            });
            return row;
        }
        case FieldKind::Color: {
            auto* button = new ColorButton;
            button->setColor(toQColor(std::get<glm::vec3>(get())));
            QObject::connect(button, &ColorButton::colorEdited, button,
                             [set](const QColor& c) { set(FieldValue(fromQColor(c))); });
            refreshers.push_back([button, get] { button->setColor(toQColor(std::get<glm::vec3>(get()))); });
            return button;
        }
        case FieldKind::String: {
            auto* edit = new QLineEdit(QString::fromStdString(std::get<std::string>(get())));
            QObject::connect(edit, &QLineEdit::editingFinished, edit,
                             [edit, set] { set(FieldValue(edit->text().toStdString())); });
            refreshers.push_back([edit, get] {
                if (editing(edit)) return;
                const QSignalBlocker block(edit);
                edit->setText(QString::fromStdString(std::get<std::string>(get())));
            });
            return edit;
        }
    }
    return new QWidget;
}

} // namespace lumora::editor
