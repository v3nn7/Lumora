#include "common/ProjectUi.hpp"

#include "common/Theme.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace lumora::app {

// "/" separators everywhere, as Qt reports paths (QFileInfo, QDir), so they compare equal on Windows.
QString toQString(const std::filesystem::path& p) { return QString::fromStdU16String(p.generic_u16string()); }
std::filesystem::path toPath(const QString& s) { return std::filesystem::path(s.toStdU16String()); }

QPixmap placeholderThumbnail(const QString& name, const QSize& size) {
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Deterministic hue per project name.
    const int hue = static_cast<int>(qHash(name) % 360u);
    QLinearGradient g(0, 0, size.width(), size.height());
    g.setColorAt(0.0, QColor::fromHsv(hue, 120, 90));
    g.setColorAt(1.0, QColor::fromHsv((hue + 40) % 360, 140, 40));
    p.fillRect(pm.rect(), g);
    // Subtle horizon line suggests a 3D scene.
    p.setPen(QPen(QColor(255, 255, 255, 30), 1));
    p.drawLine(0, size.height() * 2 / 3, size.width(), size.height() * 2 / 3);
    QString initials;
    for (const QString& part : name.split(QRegularExpression(QStringLiteral("[\\s_-]+")), Qt::SkipEmptyParts)) {
        initials += part.front().toUpper();
        if (initials.size() == 2) break;
    }
    QFont f = p.font();
    f.setPixelSize(size.height() / 3);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 220));
    p.drawText(pm.rect(), Qt::AlignCenter, initials.isEmpty() ? QStringLiteral("?") : initials);
    return pm;
}

QPixmap projectThumbnail(const QString& projectRoot, const QString& name, const QSize& size) {
    const QString path = projectRoot + QStringLiteral("/Saved/Thumbnail.png");
    QPixmap pm;
    if (QFileInfo::exists(path) && pm.load(path)) {
        return pm.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
            .copy(QRect(QPoint(0, 0), size));
    }
    return placeholderThumbnail(name, size);
}

QString relativeTimeString(qint64 unixSeconds) {
    if (unixSeconds <= 0) return QStringLiteral("Never opened");
    const qint64 delta = QDateTime::currentSecsSinceEpoch() - unixSeconds;
    if (delta < 60) return QStringLiteral("Just now");
    if (delta < 3600) return QStringLiteral("%1 min ago").arg(delta / 60);
    if (delta < 86400) return QStringLiteral("%1 h ago").arg(delta / 3600);
    if (delta < 86400 * 2) return QStringLiteral("Yesterday");
    if (delta < 86400 * 30) return QStringLiteral("%1 days ago").arg(delta / 86400);
    return QDateTime::fromSecsSinceEpoch(unixSeconds).toString(QStringLiteral("d MMM yyyy"));
}

ProjectSettingsDialog::ProjectSettingsDialog(Project& project, QWidget* parent) : QDialog(parent), project_(project) {
    setWindowTitle(tr("Project Settings"));
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(14);

    auto* title = new QLabel(tr("Project Settings"));
    title->setProperty("heading", true);
    layout->addWidget(title);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    name_ = new QLineEdit(QString::fromStdString(project.name()));
    description_ = new QPlainTextEdit(QString::fromStdString(project.description()));
    description_->setFixedHeight(80);
    defaultScene_ = new QComboBox;
    defaultScene_->setEditable(false);
    const QString root = toQString(project.rootDir());
    QDirIterator it(toQString(project.contentDir()), {QStringLiteral("*.lscene")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        defaultScene_->addItem(QDir(root).relativeFilePath(it.next()));
    }
    const QString current = QString::fromStdString(project.defaultScene());
    if (defaultScene_->findText(current) < 0) defaultScene_->addItem(current);
    defaultScene_->setCurrentText(current);

    auto* location = new QLabel(root);
    location->setProperty("muted", true);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* version = new QLabel(QString::fromStdString(project.engineVersion()));
    version->setProperty("muted", true);
    auto* created = new QLabel(project.createdUnix() > 0
                                   ? QDateTime::fromSecsSinceEpoch(project.createdUnix()).toString(Qt::TextDate)
                                   : tr("Unknown"));
    created->setProperty("muted", true);

    form->addRow(tr("Name"), name_);
    form->addRow(tr("Description"), description_);
    form->addRow(tr("Default scene"), defaultScene_);
    form->addRow(tr("Location"), location);
    form->addRow(tr("Engine version"), version);
    form->addRow(tr("Created"), created);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setProperty("accent", true);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProjectSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ProjectSettingsDialog::accept() {
    const std::string name = name_->text().trimmed().toStdString();
    if (!Project::isValidName(name)) {
        QMessageBox::warning(this, tr("Invalid name"),
                             tr("Use letters, digits, spaces, '-' or '_' (max 64 characters)."));
        return;
    }
    project_.setName(name);
    project_.setDescription(description_->toPlainText().toStdString());
    project_.setDefaultScene(defaultScene_->currentText().toStdString());
    if (auto saved = project_.save(); !saved) {
        QMessageBox::critical(this, tr("Save failed"), QString::fromStdString(saved.error().message));
        return;
    }
    QDialog::accept();
}

} // namespace lumora::app
