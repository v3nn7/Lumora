#include "hub/NewProjectDialog.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "core/Log.hpp"
#include "platform/Paths.hpp"
#include "project/ProjectTemplates.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Scene.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace lumora::app {

namespace {
constexpr QSize kPreviewSize{224, 126};
}

NewProjectDialog::NewProjectDialog(AppContext& app, QWidget* parent) : QDialog(parent), app_(app) {
    setWindowTitle(tr("New Project"));
    resize(820, 560);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(14);

    auto* title = new QLabel(tr("Create a new project"));
    title->setProperty("heading", true);
    layout->addWidget(title);
    auto* subtitle = new QLabel(tr("Choose a template to start from. You can change everything later."));
    subtitle->setProperty("muted", true);
    layout->addWidget(subtitle);

    auto* sectionTemplates = new QLabel(tr("Templates"));
    sectionTemplates->setProperty("section", true);
    layout->addWidget(sectionTemplates);

    templates_ = new QListWidget;
    templates_->setViewMode(QListView::IconMode);
    templates_->setIconSize(kPreviewSize);
    templates_->setGridSize(QSize(kPreviewSize.width() + 24, kPreviewSize.height() + 44));
    templates_->setMovement(QListView::Static);
    templates_->setResizeMode(QListView::Adjust);
    templates_->setWrapping(false);
    templates_->setFixedHeight(kPreviewSize.height() + 64);
    templates_->setSpacing(4);
    templates_->setStyleSheet(QStringLiteral("QListWidget { background: transparent; } "
                                             "QListWidget::item { border-radius: 8px; padding: 6px; color: #d7dae0; } "
                                             "QListWidget::item:selected { background: #233a63; border: 1px solid #4f8cff; }"));
    for (const ProjectTemplateInfo& info : projectTemplates()) {
        auto* item = new QListWidgetItem(QIcon(placeholderThumbnail(QString::fromLatin1(info.displayName), kPreviewSize)),
                                         QString::fromLatin1(info.displayName));
        item->setData(Qt::UserRole, static_cast<int>(info.id));
        item->setToolTip(QString::fromLatin1(info.description));
        templates_->addItem(item);
    }
    templates_->setCurrentRow(1);
    layout->addWidget(templates_);
    templateDescription_ = new QLabel;
    templateDescription_->setProperty("muted", true);
    templateDescription_->setWordWrap(true);
    layout->addWidget(templateDescription_);

    auto* sectionDetails = new QLabel(tr("Project"));
    sectionDetails->setProperty("section", true);
    layout->addWidget(sectionDetails);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    name_ = new QLineEdit(QStringLiteral("MyProject"));
    name_->setPlaceholderText(tr("Project name"));
    location_ = new QLineEdit(toQString(paths::defaultProjectsDir()));
    auto* browse = new QPushButton(theme::icon(QStringLiteral("open")), tr("Browse…"));
    grid->addWidget(new QLabel(tr("Name")), 0, 0);
    grid->addWidget(name_, 0, 1, 1, 2);
    grid->addWidget(new QLabel(tr("Location")), 1, 0);
    grid->addWidget(location_, 1, 1);
    grid->addWidget(browse, 1, 2);
    layout->addLayout(grid);

    preview_ = new QLabel;
    preview_->setProperty("muted", true);
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(preview_);
    error_ = new QLabel;
    error_->setStyleSheet(QStringLiteral("color: #ec6a5e;"));
    layout->addWidget(error_);
    layout->addStretch();

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* cancel = new QPushButton(tr("Cancel"));
    createButton_ = new QPushButton(tr("Create Project"));
    createButton_->setProperty("accent", true);
    createButton_->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(createButton_);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(createButton_, &QPushButton::clicked, this, &NewProjectDialog::create);
    connect(name_, &QLineEdit::textChanged, this, &NewProjectDialog::updateState);
    connect(location_, &QLineEdit::textChanged, this, &NewProjectDialog::updateState);
    connect(templates_, &QListWidget::currentRowChanged, this, &NewProjectDialog::updateState);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Project location"), location_->text());
        if (!dir.isEmpty()) location_->setText(dir);
    });
    updateState();
    // Render previews after the dialog is visible so opening it never feels blocked.
    QTimer::singleShot(0, this, &NewProjectDialog::renderPreviews);
}

ProjectTemplate NewProjectDialog::selectedTemplate() const {
    const QListWidgetItem* item = templates_->currentItem();
    return item ? static_cast<ProjectTemplate>(item->data(Qt::UserRole).toInt()) : ProjectTemplate::Empty;
}

QString NewProjectDialog::targetDirectory() const {
    return QDir(location_->text().trimmed()).filePath(name_->text().trimmed());
}

void NewProjectDialog::updateState() {
    for (const ProjectTemplateInfo& info : projectTemplates()) {
        if (info.id == selectedTemplate()) templateDescription_->setText(QString::fromLatin1(info.description));
    }
    const QString name = name_->text().trimmed();
    QString problem;
    if (!Project::isValidName(name.toStdString())) {
        problem = tr("Name may contain letters, digits, spaces, '-' and '_' (max 64).");
    } else if (location_->text().trimmed().isEmpty()) {
        problem = tr("Choose a location.");
    } else if (QFileInfo(targetDirectory()).exists() && !QDir(targetDirectory()).isEmpty()) {
        problem = tr("A non-empty folder with this name already exists.");
    }
    preview_->setText(tr("Will be created in  %1").arg(QDir::toNativeSeparators(targetDirectory())));
    error_->setText(problem);
    createButton_->setEnabled(problem.isEmpty());
}

void NewProjectDialog::renderPreviews() {
    render::Renderer* renderer = app_.renderer();
    if (renderer == nullptr) return;  // placeholders stay; the reason is logged
    for (int row = 0; row < templates_->count(); ++row) {
        QListWidgetItem* item = templates_->item(row);
        auto scene = createTemplateScene(static_cast<ProjectTemplate>(item->data(Qt::UserRole).toInt()));
        scene->updateWorldTransforms();
        const float aspect = static_cast<float>(kPreviewSize.width()) / kPreviewSize.height();
        auto image = renderer->capture(*scene, sceneCameraView(*scene, aspect),
                                       {static_cast<std::uint32_t>(kPreviewSize.width() * 2),
                                        static_cast<std::uint32_t>(kPreviewSize.height() * 2)},
                                       {});
        if (!image) {
            log::warn("Hub", "template preview failed: {}", image.error().message);
            continue;
        }
        const QImage qimg(image->rgba.data(), static_cast<int>(image->width), static_cast<int>(image->height),
                          QImage::Format_RGBA8888);
        item->setIcon(QIcon(QPixmap::fromImage(qimg.copy()).scaled(kPreviewSize, Qt::KeepAspectRatio,
                                                                    Qt::SmoothTransformation)));
    }
}

void NewProjectDialog::create() {
    const QString name = name_->text().trimmed();
    auto project = Project::create(toPath(targetDirectory()), name.toStdString(), selectedTemplate());
    if (!project) {
        QMessageBox::critical(this, tr("Could not create project"), QString::fromStdString(project.error().message));
        return;
    }
    created_ = toQString(project->descriptorPath());
    accept();
}

} // namespace lumora::app
