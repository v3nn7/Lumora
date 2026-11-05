#include "common/Snapshot.hpp"

#include "common/VulkanViewport.hpp"

#include <QPainter>
#include <QPixmap>
#include <QWidget>

namespace lumora::app {

namespace {
constexpr const char* kViewportProperty = "lumoraViewport";
}

void registerViewportContainer(QWidget* container, VulkanViewport* viewport) {
    container->setProperty(kViewportProperty, QVariant::fromValue<QObject*>(viewport));
}

QImage snapshotWidget(QWidget* widget) {
    const qreal dpr = widget->devicePixelRatioF();
    QImage image = widget->grab().toImage();
    QPainter painter(&image);
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        auto* viewport = qobject_cast<VulkanViewport*>(child->property(kViewportProperty).value<QObject*>());
        if (viewport == nullptr || !child->isVisible()) continue;
        const QSize pixels = child->size() * dpr;
        const QImage frame = viewport->captureFrame(pixels);
        if (frame.isNull()) continue;
        painter.drawImage(QRectF(QPointF(child->mapTo(widget, QPoint(0, 0))) * dpr, QSizeF(pixels)), frame);
    }
    return image;
}

} // namespace lumora::app
