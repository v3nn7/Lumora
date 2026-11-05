#include "hub/ProjectDetailsPanel.hpp"

#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "core/Version.hpp"
#include "hub/ProjectListModel.hpp"

#include <QDateTime>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace lumora::app {

namespace {

QPixmap rounded(const QPixmap& source, const QSize& size, qreal radius) {
    QPixmap out(size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), radius, radius);
    p.setClipPath(clip);
    p.drawPixmap(QRect(QPoint(0, 0), size), source.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    return out;
}

/// Scenes and total size, bounded so a huge folder never stalls the Hub.
std::pair<int, qint64> scanProject(const QString& root) {
    int scenes = 0;
    qint64 bytes = 0;
    int visited = 0;
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && visited < 20000) {
        it.next();
        ++visited;
        bytes += it.fileInfo().size();
        if (it.fileInfo().suffix() == QLatin1String("lscene")) ++scenes;
    }
    return {scenes, bytes};
}

QLabel* valueLabel() {
    auto* l = new QLabel;
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet(QStringLiteral("color: #c9ced6;"));
    return l;
}

} // namespace

ProjectDetailsPanel::ProjectDetailsPanel(QWidget* parent) : QWidget(parent) {
    setFixedWidth(330);
    setObjectName(QStringLiteral("detailsPanel"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral("#detailsPanel { background: #15171c; border: 1px solid #262a32; border-radius: 14px; }"));
    // Content scrolls when the window is short; the action buttons stay pinned at the bottom.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    thumbnail_ = new QLabel;
    thumbnail_->setFixedSize(290, 163);
    layout->addWidget(thumbnail_);
    name_ = new QLabel;
    name_->setWordWrap(true);
    name_->setStyleSheet(QStringLiteral("font-size: 14pt; font-weight: 700; color: #f2f4f7;"));
    layout->addWidget(name_);
    description_ = new QLabel;
    description_->setWordWrap(true);
    description_->setProperty("muted", true);
    layout->addWidget(description_);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    location_ = valueLabel();
    engine_ = valueLabel();
    created_ = valueLabel();
    opened_ = valueLabel();
    contents_ = valueLabel();
    for (const auto& [text, label] : {std::pair{tr("Location"), location_}, std::pair{tr("Engine"), engine_},
                                      std::pair{tr("Created"), created_}, std::pair{tr("Opened"), opened_},
                                      std::pair{tr("Contents"), contents_}}) {
        auto* key = new QLabel(text);
        key->setProperty("muted", true);
        form->addRow(key, label);
    }
    layout->addSpacing(4);
    layout->addLayout(form);
    layout->addStretch();

    open_ = new QPushButton(theme::icon(QStringLiteral("open")), tr("Open Project"));
    open_->setProperty("accent", true);
    open_->setMinimumHeight(36);
    outer->addWidget(open_);
    auto* row = new QHBoxLayout;
    settings_ = new QPushButton(theme::icon(QStringLiteral("settings")), tr("Settings"));
    auto* reveal = new QPushButton(theme::icon(QStringLiteral("folder")), tr("Show"));
    auto* remove = new QPushButton(theme::icon(QStringLiteral("close")), QString());
    remove->setToolTip(tr("Remove from list (files stay on disk)"));
    row->addWidget(settings_, 1);
    row->addWidget(reveal, 1);
    row->addWidget(remove);
    outer->addLayout(row);

    connect(open_, &QPushButton::clicked, this, [this] { emit openRequested(descriptor_); });
    connect(settings_, &QPushButton::clicked, this, [this] { emit settingsRequested(descriptor_); });
    connect(reveal, &QPushButton::clicked, this, [this] { emit revealRequested(root_); });
    connect(remove, &QPushButton::clicked, this, [this] { emit removeRequested(descriptor_); });
}

void ProjectDetailsPanel::showProject(const QModelIndex& index) {
    if (!index.isValid()) {
        hide();
        return;
    }
    descriptor_ = index.data(ProjectListModel::DescriptorRole).toString();
    root_ = index.data(ProjectListModel::RootRole).toString();
    const bool exists = index.data(ProjectListModel::ExistsRole).toBool();
    const QString name = index.data(ProjectListModel::NameRole).toString();

    thumbnail_->setPixmap(rounded(projectThumbnail(root_, name, QSize(596, 336)), thumbnail_->size(), 10));
    name_->setText(name);
    const QString description = index.data(ProjectListModel::DescriptionRole).toString();
    description_->setText(description.isEmpty() ? tr("No description") : description);
    // Long paths: show the tail, full path in the tooltip.
    const QString native = QDir::toNativeSeparators(root_);
    location_->setText(QFontMetrics(location_->font()).elidedText(native, Qt::ElideMiddle, 200));
    location_->setToolTip(native);

    const QString version = index.data(ProjectListModel::EngineVersionRole).toString();
    if (!exists) {
        engine_->setText(tr("<span style='color:#ec6a5e'>Project files not found</span>"));
    } else if (version.isEmpty() || version == QLatin1String(version::kString)) {
        engine_->setText(QStringLiteral("%1 %2").arg(QString::fromLatin1(version::kEngineName), QString::fromLatin1(version::kString)));
    } else {
        engine_->setText(tr("Saved with %1 — will be upgraded to %2 on save")
                             .arg(version, QString::fromLatin1(version::kString)));
    }
    const qint64 created = index.data(ProjectListModel::CreatedRole).toLongLong();
    created_->setText(created > 0 ? QLocale().toString(QDateTime::fromSecsSinceEpoch(created), QLocale::ShortFormat) : tr("Unknown"));
    opened_->setText(relativeTimeString(index.data(ProjectListModel::LastOpenedRole).toLongLong()));
    if (exists) {
        const auto [scenes, bytes] = scanProject(root_);
        contents_->setText((scenes == 1 ? tr("1 scene") : tr("%1 scenes").arg(scenes)) + QStringLiteral(" · ") +
                           QLocale().formattedDataSize(bytes));
    } else {
        contents_->setText(QStringLiteral("—"));
    }
    open_->setEnabled(exists);
    settings_->setEnabled(exists);
    show();
}

} // namespace lumora::app
