#pragma once

#include "common/ModuleExport.hpp"

#include <QStyledItemDelegate>

namespace lumora::app {

/// Paints a project as a card: rounded thumbnail, name, path and last-opened time.
class LUMORA_HUB_API ProjectCardDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr QSize kCardSize{278, 238};
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

} // namespace lumora::app
