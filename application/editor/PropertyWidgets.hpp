#pragma once

#include "common/ModuleExport.hpp"

#include "scene/ComponentRegistry.hpp"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QToolButton;
class QVBoxLayout;

namespace lumora::editor {

/// Label that scrubs an attached spin box when dragged horizontally (Shift = fine).
class LUMORA_EDITOR_API ScrubLabel final : public QLabel {
    Q_OBJECT
public:
    ScrubLabel(const QString& text, QDoubleSpinBox* target, QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QDoubleSpinBox* target_;
    QPointF last_;
    bool dragging_ = false;
};

class LUMORA_EDITOR_API ColorButton final : public QPushButton {
    Q_OBJECT
public:
    explicit ColorButton(QWidget* parent = nullptr);
    void setColor(const QColor& c);
    [[nodiscard]] QColor color() const noexcept { return color_; }

signals:
    void colorEdited(const QColor& color);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    void pick();
    QColor color_;
};

/// Collapsible inspector section with an optional remove action.
class LUMORA_EDITOR_API Section final : public QWidget {
    Q_OBJECT
public:
    Section(const QString& title, const QIcon& icon, bool removable, QWidget* parent = nullptr);
    [[nodiscard]] QWidget* body() const noexcept { return body_; }

signals:
    void removeRequested();

private:
    QToolButton* header_ = nullptr;
    QLabel* chevron_ = nullptr;
    QWidget* body_ = nullptr;
};

using FieldGetter = std::function<FieldValue()>;
using FieldSetter = std::function<void(const FieldValue&)>;
using Refresher = std::function<void()>;
/// Opens an asset chooser for a field; returns the chosen reference or nothing if cancelled.
using AssetPicker = std::function<std::optional<std::string>(QWidget* parent, const FieldDesc& field)>;

/// Builds an editor widget for a reflected field. `refreshers` receives a callback that
/// re-reads the value (skipped while the user is editing that widget).
[[nodiscard]] QWidget* createFieldEditor(const FieldDesc& desc, FieldGetter get, FieldSetter set,
                                         std::vector<Refresher>& refreshers, const AssetPicker& pickAsset = {});

} // namespace lumora::editor
