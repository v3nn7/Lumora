#include "hub/HeroBanner.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

namespace lumora::app {

HeroBanner::HeroBanner(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setMinimumHeight(176);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HeroBanner::setBackground(QImage image) {
    background_ = std::move(image);
    update();
}

void HeroBanner::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(r, 14, 14);
    p.setClipPath(clip);

    if (!background_.isNull()) {
        // "Cover" fit: fill the banner, crop the overflow around the centre.
        const QSizeF img = background_.size() / background_.devicePixelRatio();
        const qreal scale = std::max(r.width() / img.width(), r.height() / img.height());
        const QSizeF drawn = img * scale;
        p.drawImage(QRectF(r.center() - QPointF(drawn.width() / 2, drawn.height() / 2), drawn), background_);
    } else {
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, QColor(0x1b, 0x2a, 0x52));
        g.setColorAt(0.55, QColor(0x2b, 0x25, 0x5e));
        g.setColorAt(1.0, QColor(0x12, 0x3a, 0x5a));
        p.fillRect(r, g);
        for (const auto& [pos, radius, color] : {std::tuple{QPointF(0.8, 0.2), 0.55, QColor(124, 196, 255, 90)},
                                                 std::tuple{QPointF(0.95, 1.0), 0.45, QColor(111, 91, 255, 80)}}) {
            QRadialGradient glow(QPointF(r.width() * pos.x(), r.height() * pos.y()), r.width() * radius);
            glow.setColorAt(0.0, color);
            glow.setColorAt(1.0, Qt::transparent);
            p.fillRect(r, glow);
        }
    }

    // Legibility scrim: strong on the left where the text sits, clear on the right.
    QLinearGradient scrim(r.topLeft(), r.topRight());
    scrim.setColorAt(0.0, QColor(10, 12, 18, 235));
    scrim.setColorAt(0.45, QColor(10, 12, 18, 150));
    scrim.setColorAt(1.0, QColor(10, 12, 18, 10));
    p.fillRect(r, scrim);
    QLinearGradient bottom(r.topLeft(), r.bottomLeft());
    bottom.setColorAt(0.6, Qt::transparent);
    bottom.setColorAt(1.0, QColor(10, 12, 18, 120));
    p.fillRect(r, bottom);

    p.setClipping(false);
    p.setPen(QPen(QColor(255, 255, 255, 22), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 14, 14);
}

} // namespace lumora::app
