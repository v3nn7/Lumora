#include "hub/ProjectCardDelegate.hpp"

#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "hub/ProjectListModel.hpp"

#include <QPainter>
#include <QPainterPath>

namespace lumora::app {

QSize ProjectCardDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const { return kCardSize; }

void ProjectCardDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF card = QRectF(option.rect).adjusted(8, 8, -8, -8);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool exists = index.data(ProjectListModel::ExistsRole).toBool();

    QPainterPath shape;
    shape.addRoundedRect(card, 10, 10);
    p->fillPath(shape, hovered || selected ? QColor(0x22, 0x25, 0x2c) : QColor(0x1a, 0x1c, 0x21));
    p->setPen(QPen(selected ? theme::kAccent : QColor(0x2c, 0x30, 0x38), selected ? 2.0 : 1.0));
    p->drawPath(shape);

    // Thumbnail (16:9) clipped to the card's top corners.
    const QRectF thumbRect(card.left() + 1, card.top() + 1, card.width() - 2, (card.width() - 2) * 9.0 / 16.0);
    QPainterPath clip;
    clip.addRoundedRect(thumbRect, 9, 9);
    clip.addRect(thumbRect.adjusted(0, thumbRect.height() / 2, 0, 0));
    p->save();
    p->setClipPath(clip);
    const QPixmap thumb = index.data(ProjectListModel::ThumbnailRole).value<QPixmap>();
    p->drawPixmap(thumbRect, thumb, QRectF(thumb.rect()));
    if (!exists) {
        p->fillRect(thumbRect, QColor(0, 0, 0, 150));
        const QRect iconRect(static_cast<int>(thumbRect.center().x()) - 16, static_cast<int>(thumbRect.center().y()) - 16,
                             32, 32);
        theme::icon(QStringLiteral("warning")).paint(p, iconRect);
    }
    p->restore();

    const QRectF text = QRectF(card.left() + 12, thumbRect.bottom() + 8, card.width() - 24, card.bottom() - thumbRect.bottom() - 12);
    QFont nameFont = option.font;
    nameFont.setWeight(QFont::DemiBold);
    nameFont.setPointSizeF(option.font.pointSizeF() + 1.0);
    p->setFont(nameFont);
    p->setPen(exists ? theme::kText : theme::kTextMuted);
    const QFontMetricsF nameMetrics(nameFont);
    p->drawText(QRectF(text.left(), text.top(), text.width(), nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                nameMetrics.elidedText(index.data(ProjectListModel::NameRole).toString(), Qt::ElideRight, text.width()));

    QFont small = option.font;
    small.setPointSizeF(option.font.pointSizeF() - 0.5);
    p->setFont(small);
    const QFontMetricsF smallMetrics(small);
    p->setPen(theme::kTextMuted);
    const QString subtitle = exists ? relativeTimeString(index.data(ProjectListModel::LastOpenedRole).toLongLong())
                                    : tr("Project not found");
    p->drawText(QRectF(text.left(), text.top() + nameMetrics.height() + 2, text.width(), smallMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter, subtitle);
    p->setPen(QColor(0x6b, 0x71, 0x7c));
    p->drawText(QRectF(text.left(), text.top() + nameMetrics.height() + smallMetrics.height() + 4, text.width(),
                       smallMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                smallMetrics.elidedText(index.data(ProjectListModel::RootRole).toString(), Qt::ElideMiddle, text.width()));
    p->restore();
}

} // namespace lumora::app
