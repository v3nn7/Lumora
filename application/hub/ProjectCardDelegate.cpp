#include "hub/ProjectCardDelegate.hpp"

#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "core/Version.hpp"
#include "hub/ProjectListModel.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace lumora::app {

QSize ProjectCardDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const { return kCardSize; }

void ProjectCardDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool exists = index.data(ProjectListModel::ExistsRole).toBool();
    // Hovered cards lift slightly.
    const QRectF card = QRectF(option.rect).adjusted(9, hovered ? 7 : 9, -9, hovered ? -11 : -9);
    constexpr qreal kRadius = 12.0;

    // Soft drop shadow (layered translucent outlines).
    for (int i = 1; i <= 6; ++i) {
        QPainterPath shadow;
        shadow.addRoundedRect(card.adjusted(-i, -i + 3, i, i + 3), kRadius + i, kRadius + i);
        p->fillPath(shadow, QColor(0, 0, 0, hovered ? 14 : 9));
    }

    QPainterPath shape;
    shape.addRoundedRect(card, kRadius, kRadius);
    QLinearGradient bg(card.topLeft(), card.bottomLeft());
    bg.setColorAt(0.0, hovered || selected ? QColor(0x23, 0x27, 0x30) : QColor(0x1b, 0x1e, 0x25));
    bg.setColorAt(1.0, hovered || selected ? QColor(0x1d, 0x21, 0x29) : QColor(0x17, 0x1a, 0x20));
    p->fillPath(shape, bg);

    // Thumbnail (16:9) clipped to the card's top corners.
    const QRectF thumbRect(card.left(), card.top(), card.width(), card.width() * 9.0 / 16.0);
    QPainterPath thumbArea;
    thumbArea.addRect(thumbRect);
    p->save();
    p->setClipPath(shape.intersected(thumbArea));  // rounded top corners, square bottom edge
    const QPixmap thumb = index.data(ProjectListModel::ThumbnailRole).value<QPixmap>();
    p->drawPixmap(thumbRect, thumb, QRectF(thumb.rect()));
    QLinearGradient scrim(thumbRect.topLeft(), thumbRect.bottomLeft());
    scrim.setColorAt(0.55, Qt::transparent);
    scrim.setColorAt(1.0, QColor(10, 12, 16, 150));
    p->fillRect(thumbRect, scrim);
    if (!exists) {
        p->fillRect(thumbRect, QColor(12, 13, 17, 225));
        const QRect iconRect(static_cast<int>(thumbRect.center().x()) - 16, static_cast<int>(thumbRect.center().y()) - 16,
                             32, 32);
        theme::icon(QStringLiteral("warning")).paint(p, iconRect);
    }
    p->restore();
    p->setPen(QPen(QColor(255, 255, 255, 14), 1));
    p->drawLine(thumbRect.bottomLeft(), thumbRect.bottomRight());

    // Status chip on the thumbnail: missing project, or saved by another engine version.
    const QString version = index.data(ProjectListModel::EngineVersionRole).toString();
    QString chipText;
    QColor chipColor;
    if (!exists) {
        chipText = tr("Missing");
        chipColor = QColor(236, 106, 94, 225);
    } else if (!version.isEmpty() && version != QLatin1String(version::kString)) {
        chipText = tr("Engine %1").arg(version);
        chipColor = QColor(232, 184, 74, 225);
    }
    if (!chipText.isEmpty()) {
        QFont chipFont = option.font;
        chipFont.setPointSizeF(option.font.pointSizeF() - 1.0);
        chipFont.setWeight(QFont::DemiBold);
        const QFontMetricsF fm(chipFont);
        const QRectF chip(thumbRect.left() + 10, thumbRect.top() + 10, fm.horizontalAdvance(chipText) + 16, fm.height() + 6);
        QPainterPath chipPath;
        chipPath.addRoundedRect(chip, chip.height() / 2, chip.height() / 2);
        p->fillPath(chipPath, chipColor);
        p->setFont(chipFont);
        p->setPen(exists ? QColor(30, 24, 10) : QColor(Qt::white));
        p->drawText(chip, Qt::AlignCenter, chipText);
    }

    // Text block.
    const QRectF text(card.left() + 14, thumbRect.bottom() + 10, card.width() - 28, card.bottom() - thumbRect.bottom() - 14);
    QFont nameFont = option.font;
    nameFont.setWeight(QFont::DemiBold);
    nameFont.setPointSizeF(option.font.pointSizeF() + 1.5);
    const QFontMetricsF nameMetrics(nameFont);
    p->setFont(nameFont);
    p->setPen(exists ? QColor(0xf0, 0xf2, 0xf5) : theme::kTextMuted);
    p->drawText(QRectF(text.left(), text.top(), text.width(), nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                nameMetrics.elidedText(index.data(ProjectListModel::NameRole).toString(), Qt::ElideRight, text.width()));

    QFont small = option.font;
    small.setPointSizeF(option.font.pointSizeF() - 0.5);
    const QFontMetricsF smallMetrics(small);
    p->setFont(small);
    const qreal row2 = text.top() + nameMetrics.height() + 4;
    theme::icon(QStringLiteral("clock")).paint(p, QRect(int(text.left()), int(row2 + (smallMetrics.height() - 13) / 2), 13, 13));
    p->setPen(theme::kTextMuted);
    const QString subtitle = exists ? relativeTimeString(index.data(ProjectListModel::LastOpenedRole).toLongLong())
                                    : tr("Project not found");
    p->drawText(QRectF(text.left() + 18, row2, text.width() - 18, smallMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                subtitle);
    p->setPen(QColor(0x62, 0x69, 0x75));
    p->drawText(QRectF(text.left(), row2 + smallMetrics.height() + 3, text.width(), smallMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                smallMetrics.elidedText(index.data(ProjectListModel::RootRole).toString(), Qt::ElideMiddle, text.width()));

    // Border: accent when selected, subtle accent glow on hover.
    if (selected) {
        p->setPen(QPen(theme::kAccent, 2.0));
    } else if (hovered) {
        QColor glow = theme::kAccent;
        glow.setAlpha(120);
        p->setPen(QPen(glow, 1.2));
    } else {
        p->setPen(QPen(QColor(0x2a, 0x2e, 0x37), 1.0));
    }
    p->setBrush(Qt::NoBrush);
    p->drawPath(shape);
    p->restore();
}

} // namespace lumora::app
