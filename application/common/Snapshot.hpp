#pragma once

#include <QImage>

class QWidget;

namespace lumora::app {

class VulkanViewport;

/// Marks `container` (from QWidget::createWindowContainer) as hosting `viewport`, so snapshots
/// can composite the GPU image that a widget grab cannot see.
void registerViewportContainer(QWidget* container, VulkanViewport* viewport);

/// Grabs a widget tree including any embedded Vulkan viewports (rendered offscreen at the
/// container's pixel size with the viewport's current camera).
[[nodiscard]] QImage snapshotWidget(QWidget* widget);

} // namespace lumora::app
