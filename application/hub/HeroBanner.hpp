#pragma once

#include "common/ModuleExport.hpp"

#include <QImage>
#include <QWidget>

namespace lumora::app {

/// Rounded banner at the top of the Hub. Shows a frame rendered by the engine itself when the
/// GPU is available (falls back to a painted gradient), darkened on the left for legible text.
class LUMORA_HUB_API HeroBanner final : public QWidget {
    Q_OBJECT
public:
    explicit HeroBanner(QWidget* parent = nullptr);
    void setBackground(QImage image);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage background_;
};

} // namespace lumora::app
