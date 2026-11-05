#pragma once

#include <QStyledItemDelegate>

namespace lumora::app {

/// Paints a project as a card: rounded thumbnail, name, path and last-opened time.
class ProjectCardDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr QSize kCardSize{260, 214};
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

} // namespace lumora::app
