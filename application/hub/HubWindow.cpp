#include "hub/HubWindow.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "hub/NewProjectDialog.hpp"
#include "hub/ProjectCardDelegate.hpp"
#include "hub/ProjectListModel.hpp"
#include "renderer/RenderContext.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace lumora::app {

HubWindow::HubWindow(AppContext& app, QWidget* parent) : QWidget(parent), app_(app) {
    setWindowTitle(tr("Lumora Hub"));
    setWindowIcon(theme::icon(QStringLiteral("logo")));
    resize(1180, 740);
    setMinimumSize(860, 560);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildSidebar());

    pages_ = new QStackedWidget;
    pages_->addWidget(buildProjectsPage());
    pages_->addWidget(buildAboutPage());
    root->addWidget(pages_, 1);
    updateEmptyState();
}

QWidget* HubWindow::buildSidebar() {
    auto* sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("hubSidebar"));
    sidebar->setFixedWidth(232);
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setStyleSheet(QStringLiteral(
        "#hubSidebar { background: #16181c; border-right: 1px solid #23262d; }"
        "#hubSidebar QPushButton { text-align: left; padding: 9px 14px; border: none; border-radius: 7px;"
        "  background: transparent; color: #aeb4bf; font-weight: 500; }"
        "#hubSidebar QPushButton:hover { background: #20232a; color: #d7dae0; }"
        "#hubSidebar QPushButton:checked { background: #22304a; color: #ffffff; }"));
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(16, 22, 16, 18);
    layout->setSpacing(4);

    auto* brand = new QHBoxLayout;
    auto* logo = new QLabel;
    logo->setPixmap(theme::icon(QStringLiteral("logo")).pixmap(30, 30));
    auto* word = new QLabel(QStringLiteral("LUMORA"));
    word->setStyleSheet(QStringLiteral("font-size: 15pt; font-weight: 700; letter-spacing: 4px; color: #f0f2f5;"));
    brand->addWidget(logo);
    brand->addSpacing(6);
    brand->addWidget(word);
    brand->addStretch();
    layout->addLayout(brand);
    auto* tagline = new QLabel(tr("Engine Hub"));
    tagline->setProperty("muted", true);
    tagline->setContentsMargins(42, 0, 0, 0);
    layout->addWidget(tagline);
    layout->addSpacing(26);

    auto* group = new QButtonGroup(this);
    const std::pair<const char*, const char*> navItems[] = {{"Projects", "folder"}, {"About", "info"}};
    int id = 0;
    for (const auto& [label, iconName] : navItems) {
        auto* button = new QPushButton(theme::icon(QString::fromLatin1(iconName)), tr(label));
        button->setCheckable(true);
        button->setIconSize(QSize(18, 18));
        group->addButton(button, id++);
        layout->addWidget(button);
    }
    group->button(0)->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int page) {
        pages_->setCurrentIndex(page);
        if (page == 1) updateAbout();
    });

    layout->addStretch();
    auto* version = new QLabel(QStringLiteral("%1 %2 · %3")
                                   .arg(QString::fromLatin1(version::kEngineName), QString::fromLatin1(version::kString),
                                        QString::fromLatin1(version::kBuildType)));
    version->setProperty("muted", true);
    version->setWordWrap(true);
    layout->addWidget(version);
    return sidebar;
}

QWidget* HubWindow::buildProjectsPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 26, 32, 20);
    layout->setSpacing(16);

    auto* header = new QHBoxLayout;
    auto* titleBox = new QVBoxLayout;
    auto* title = new QLabel(tr("Projects"));
    title->setProperty("heading", true);
    countLabel_ = new QLabel;
    countLabel_->setProperty("muted", true);
    titleBox->addWidget(title);
    titleBox->addWidget(countLabel_);
    header->addLayout(titleBox);
    header->addStretch();

    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Search projects"));
    search->addAction(theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    search->setClearButtonEnabled(true);
    search->setFixedWidth(240);
    auto* sort = new QComboBox;
    sort->addItems({tr("Last opened"), tr("Name")});
    auto* open = new QPushButton(theme::icon(QStringLiteral("open")), tr("Open…"));
    auto* create = new QPushButton(theme::icon(QStringLiteral("plus")), tr("New Project"));
    create->setProperty("accent", true);
    header->addWidget(search);
    header->addWidget(sort);
    header->addSpacing(8);
    header->addWidget(open);
    header->addWidget(create);
    layout->addLayout(header);

    model_ = new ProjectListModel(app_.recentProjects(), this);
    filter_ = new ProjectFilterModel(this);
    filter_->setSourceModel(model_);
    filter_->setSort(ProjectFilterModel::Sort::Recent);

    list_ = new QListView;
    list_->setModel(filter_);
    list_->setItemDelegate(new ProjectCardDelegate(list_));
    list_->setViewMode(QListView::IconMode);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setUniformItemSizes(true);
    list_->setGridSize(ProjectCardDelegate::kCardSize);
    list_->setMouseTracking(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setStyleSheet(QStringLiteral("QListView { background: transparent; } QListView::item { background: transparent; }"));

    // Empty state.
    auto* empty = new QWidget;
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch();
    auto* emptyIcon = new QLabel;
    emptyIcon->setPixmap(theme::icon(QStringLiteral("logo")).pixmap(72, 72));
    emptyIcon->setAlignment(Qt::AlignCenter);
    auto* emptyTitle = new QLabel(tr("No projects yet"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyTitle->setStyleSheet(QStringLiteral("font-size: 14pt; font-weight: 600;"));
    auto* emptyText = new QLabel(tr("Create a project from a template or open an existing one."));
    emptyText->setProperty("muted", true);
    emptyText->setAlignment(Qt::AlignCenter);
    auto* emptyButton = new QPushButton(theme::icon(QStringLiteral("plus")), tr("Create your first project"));
    emptyButton->setProperty("accent", true);
    emptyButton->setFixedWidth(240);
    emptyLayout->addWidget(emptyIcon);
    emptyLayout->addSpacing(8);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyText);
    emptyLayout->addSpacing(12);
    emptyLayout->addWidget(emptyButton, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();

    projectStack_ = new QStackedWidget;
    projectStack_->addWidget(list_);
    projectStack_->addWidget(empty);
    layout->addWidget(projectStack_, 1);

    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_->setSearch(text);
    });
    connect(sort, &QComboBox::currentIndexChanged, this, [this](int i) {
        filter_->setSort(i == 0 ? ProjectFilterModel::Sort::Recent : ProjectFilterModel::Sort::Name);
    });
    connect(create, &QPushButton::clicked, this, &HubWindow::newProject);
    connect(emptyButton, &QPushButton::clicked, this, &HubWindow::newProject);
    connect(open, &QPushButton::clicked, this, &HubWindow::openFromDisk);
    connect(list_, &QListView::activated, this, [this](const QModelIndex& index) {
        if (!index.data(ProjectListModel::ExistsRole).toBool()) {
            QMessageBox::warning(this, tr("Project missing"),
                                 tr("The project was moved or deleted:\n%1").arg(index.data(ProjectListModel::DescriptorRole).toString()));
            return;
        }
        emit openProjectRequested(index.data(ProjectListModel::DescriptorRole).toString());
    });
    connect(list_, &QListView::customContextMenuRequested, this, &HubWindow::showContextMenu);
    return page;
}

QWidget* HubWindow::buildAboutPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 26, 32, 20);
    layout->setSpacing(12);
    auto* title = new QLabel(tr("About Lumora"));
    title->setProperty("heading", true);
    layout->addWidget(title);
    auto* info = new QLabel(
        tr("<p><b>%1</b> %2 (%3 build)<br>Qt %4 · C++23 · Vulkan 1.4 renderer</p>"
           "<p>Engine modules: core, platform, scene/ECS (EnTT), renderer (Vulkan, render graph), "
           "project, runtime. Hub and Editor share one executable.</p>")
            .arg(QString::fromLatin1(version::kEngineName), QString::fromLatin1(version::kString),
                 QString::fromLatin1(version::kBuildType), QString::fromLatin1(qVersion())));
    info->setWordWrap(true);
    info->setTextFormat(Qt::RichText);
    layout->addWidget(info);
    auto* gpuTitle = new QLabel(tr("Graphics devices"));
    gpuTitle->setProperty("section", true);
    layout->addSpacing(8);
    layout->addWidget(gpuTitle);
    aboutGpus_ = new QLabel(tr("Querying…"));
    aboutGpus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    aboutGpus_->setStyleSheet(QStringLiteral("font-family: monospace; background: #16181c; border: 1px solid #2c3038;"
                                             "border-radius: 6px; padding: 12px;"));
    aboutGpus_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(aboutGpus_);
    layout->addStretch();
    return page;
}

void HubWindow::updateAbout() {
    if (aboutFilled_) return;
    aboutFilled_ = true;
    try {
        aboutGpus_->setText(QString::fromStdString(render::RenderContext::describeDevices(true)).trimmed());
    } catch (const std::exception& e) {
        aboutGpus_->setText(tr("Vulkan is not available: %1").arg(QString::fromUtf8(e.what())));
    }
}

void HubWindow::updateEmptyState() {
    const int count = model_->rowCount();
    projectStack_->setCurrentIndex(count == 0 ? 1 : 0);
    countLabel_->setText(count == 1 ? tr("1 project") : tr("%1 projects").arg(count));
}

void HubWindow::refresh() {
    model_->reload();
    updateEmptyState();
}

void HubWindow::newProject() {
    NewProjectDialog dialog(app_, this);
    if (dialog.exec() == QDialog::Accepted) {
        auto project = Project::load(toPath(dialog.createdDescriptor()));
        if (project) {
            app_.recentProjects().touch(project->descriptorPath(), project->name());
            app_.saveRecentProjects();
            refresh();
        }
        emit openProjectRequested(dialog.createdDescriptor());
    }
}

void HubWindow::openFromDisk() {
    const QString file = QFileDialog::getOpenFileName(this, tr("Open Lumora project"), QString(),
                                                      tr("Lumora project (*.lumora)"));
    if (file.isEmpty()) return;
    emit openProjectRequested(file);
}

void HubWindow::showContextMenu(const QPoint& pos) {
    const QModelIndex index = list_->indexAt(pos);
    if (!index.isValid()) return;
    const QString descriptor = index.data(ProjectListModel::DescriptorRole).toString();
    const QString root = index.data(ProjectListModel::RootRole).toString();
    const bool exists = index.data(ProjectListModel::ExistsRole).toBool();

    QMenu menu(this);
    QAction* open = menu.addAction(theme::icon(QStringLiteral("open")), tr("Open"));
    QAction* reveal = menu.addAction(theme::icon(QStringLiteral("folder")), tr("Show in File Manager"));
    QAction* settings = menu.addAction(theme::icon(QStringLiteral("settings")), tr("Project Settings…"));
    menu.addSeparator();
    QAction* remove = menu.addAction(theme::icon(QStringLiteral("close")), tr("Remove from List"));
    open->setEnabled(exists);
    reveal->setEnabled(exists);
    settings->setEnabled(exists);

    QAction* chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
    if (chosen == open) {
        emit openProjectRequested(descriptor);
    } else if (chosen == reveal) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(root));
    } else if (chosen == settings) {
        auto project = Project::load(toPath(descriptor));
        if (!project) {
            QMessageBox::critical(this, tr("Cannot load project"), QString::fromStdString(project.error().message));
            return;
        }
        ProjectSettingsDialog dialog(*project, this);
        if (dialog.exec() == QDialog::Accepted) {
            app_.recentProjects().touch(project->descriptorPath(), project->name());
            app_.saveRecentProjects();
            refresh();
        }
    } else if (chosen == remove) {
        // Only the list entry is removed; project files are never deleted from the Hub.
        app_.recentProjects().remove(toPath(descriptor));
        app_.saveRecentProjects();
        refresh();
    }
}

} // namespace lumora::app
