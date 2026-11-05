#include "hub/HubWindow.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "hub/HeroBanner.hpp"
#include "hub/HubSettingsPage.hpp"
#include "hub/NewProjectDialog.hpp"
#include "hub/ProjectDetailsPanel.hpp"
#include "hub/ProjectCardDelegate.hpp"
#include "hub/ProjectListModel.hpp"
#include "project/ProjectTemplates.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Scene.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace lumora::app {

namespace {

QLabel* label(const QString& text, const char* style) {
    auto* l = new QLabel(text);
    l->setStyleSheet(QString::fromLatin1(style));
    return l;
}

/// Decorative empty-state illustration: concentric rings around the Lumora mark.
class EmptyIllustration final : public QWidget {
public:
    EmptyIllustration() { setFixedSize(150, 150); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c = QRectF(rect()).center();
        for (int i = 3; i >= 1; --i) {
            QColor ring = theme::kAccent;
            ring.setAlpha(18 * i);
            p.setPen(QPen(ring, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 24.0 + i * 16.0, 24.0 + i * 16.0);
        }
        QRadialGradient glow(c, 60);
        glow.setColorAt(0.0, QColor(79, 140, 255, 70));
        glow.setColorAt(1.0, Qt::transparent);
        p.fillRect(rect(), glow);
        theme::icon(QStringLiteral("logo")).paint(&p, QRect(QPoint(int(c.x()) - 28, int(c.y()) - 28), QSize(56, 56)));
    }
};

} // namespace

HubWindow::HubWindow(AppContext& app, QWidget* parent) : QWidget(parent), app_(app) {
    setWindowTitle(tr("Lumora Hub"));
    setWindowIcon(theme::icon(QStringLiteral("logo")));
    resize(1240, 780);
    setMinimumSize(900, 600);
    setObjectName(QStringLiteral("hubRoot"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral("#hubRoot { background: #0f1014; }"));

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildSidebar());

    pages_ = new QStackedWidget;
    pages_->addWidget(buildProjectsPage());
    auto* settings = new HubSettingsPage(app_);
    connect(settings, &HubSettingsPage::recentProjectsChanged, this, &HubWindow::refresh);
    pages_->addWidget(settings);
    pages_->addWidget(buildAboutPage());
    setAcceptDrops(true);
    refresh();
    root->addWidget(pages_, 1);
    updateEmptyState();
}

void HubWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!heroRendered_) {
        heroRendered_ = true;
        // After the first paint, so the window appears immediately even while Vulkan starts.
        QTimer::singleShot(60, this, &HubWindow::renderHero);
    }
}

void HubWindow::renderHero() {
    render::Renderer* renderer = app_.renderer();
    if (renderer == nullptr) return;  // painted gradient stays; reason is in the log
    auto scene = createTemplateScene(ProjectTemplate::MaterialShowcase);
    scene->updateWorldTransforms();
    FlyCameraController camera;
    camera.fovDeg = 32.0f;
    camera.setPosition({4.6f, 1.5f, 5.4f});
    camera.lookAt({0.9f, 1.05f, 0.0f});
    const qreal dpr = devicePixelRatioF();
    const QSize size = hero_->size() * dpr;
    const std::uint32_t w = static_cast<std::uint32_t>(std::max(size.width(), 400));
    const std::uint32_t h = static_cast<std::uint32_t>(std::max(size.height(), 120));
    render::RenderSettings settings;
    settings.shadowDistance = 20.0f;
    try {
        auto image = renderer->capture(*scene, camera.view(static_cast<float>(w) / static_cast<float>(h)), {w, h}, settings);
        if (!image) return;
        QImage qimg = QImage(image->rgba.data(), static_cast<int>(image->width), static_cast<int>(image->height),
                             QImage::Format_RGBA8888)
                          .copy();
        qimg.setDevicePixelRatio(dpr);
        hero_->setBackground(std::move(qimg));
    } catch (const std::exception& e) {
        log::warn("Hub", "hero render failed: {}", e.what());
    }
}

QWidget* HubWindow::buildSidebar() {
    auto* sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("hubSidebar"));
    sidebar->setFixedWidth(248);
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setStyleSheet(QStringLiteral(
        "#hubSidebar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #171a21, stop:1 #121419);"
        "  border-right: 1px solid #22252d; }"
        "#hubSidebar QPushButton[nav=\"true\"] { text-align: left; padding: 10px 14px; border: none;"
        "  border-radius: 8px; background: transparent; color: #9aa1ad; font-weight: 500; }"
        "#hubSidebar QPushButton[nav=\"true\"]:hover { background: #1e222a; color: #e2e5ea; }"
        "#hubSidebar QPushButton[nav=\"true\"]:checked { background: #1f2b42; color: #ffffff;"
        "  border-left: 3px solid #4f8cff; padding-left: 11px; }"));
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(18, 24, 18, 20);
    layout->setSpacing(4);

    auto* brand = new QHBoxLayout;
    auto* logo = new QLabel;
    logo->setPixmap(theme::icon(QStringLiteral("logo")).pixmap(QSize(34, 34), devicePixelRatioF()));
    auto* words = new QVBoxLayout;
    words->setSpacing(0);
    words->addWidget(label(QStringLiteral("LUMORA"), "font-size: 15pt; font-weight: 800; letter-spacing: 5px; color: #f3f5f8;"));
    words->addWidget(label(tr("Engine Hub"), "color: #7c8494; font-size: 8.5pt; letter-spacing: 1px;"));
    brand->addWidget(logo);
    brand->addSpacing(8);
    brand->addLayout(words);
    brand->addStretch();
    layout->addLayout(brand);
    layout->addSpacing(26);

    auto* create = new QPushButton(theme::icon(QStringLiteral("plus")), tr("New Project"));
    create->setProperty("accent", true);
    create->setMinimumHeight(38);
    connect(create, &QPushButton::clicked, this, &HubWindow::newProject);
    layout->addWidget(create);
    layout->addSpacing(18);
    layout->addWidget(label(tr("NAVIGATION"), "color: #5d6472; font-size: 8pt; font-weight: 700; letter-spacing: 1.5px; padding-left: 4px;"));
    layout->addSpacing(4);

    auto* group = new QButtonGroup(this);
    const std::pair<const char*, const char*> navItems[] = {{"Projects", "folder"}, {"Settings", "settings"}, {"About", "info"}};
    int id = 0;
    for (const auto& [text, iconName] : navItems) {
        auto* button = new QPushButton(theme::icon(QString::fromLatin1(iconName)), tr(text));
        button->setProperty("nav", true);
        button->setCheckable(true);
        button->setIconSize(QSize(18, 18));
        group->addButton(button, id++);
        layout->addWidget(button);
    }
    group->button(0)->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int page) {
        pages_->setCurrentIndex(page);
        if (page == 2) updateAbout();
    });

    layout->addStretch();
    auto* footer = new QWidget;
    footer->setObjectName(QStringLiteral("hubFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    footer->setStyleSheet(QStringLiteral("#hubFooter { background: #181b22; border: 1px solid #252932; border-radius: 10px; }"));
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(12, 10, 12, 10);
    footerLayout->setSpacing(2);
    footerLayout->addWidget(label(QStringLiteral("%1 %2").arg(QString::fromLatin1(version::kEngineName),
                                                             QString::fromLatin1(version::kString)),
                                  "color: #d7dae0; font-weight: 600;"));
    footerLayout->addWidget(label(tr("%1 build · Vulkan 1.4 · Qt %2")
                                      .arg(QString::fromLatin1(version::kBuildType), QString::fromLatin1(qVersion())),
                                  "color: #7c8494; font-size: 8.5pt;"));
    layout->addWidget(footer);
    return sidebar;
}

QWidget* HubWindow::buildProjectsPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 18);
    layout->setSpacing(18);

    // Hero
    hero_ = new HeroBanner;
    auto* heroLayout = new QVBoxLayout(hero_);
    heroLayout->setContentsMargins(30, 24, 30, 24);
    heroLayout->setSpacing(6);
    heroTitle_ = label(tr("Welcome to Lumora"), "font-size: 21pt; font-weight: 700; color: #ffffff; background: transparent;");
    heroLayout->addWidget(heroTitle_);
    heroLayout->addWidget(label(tr("Build photoreal worlds on a Vulkan 1.4 renderer.\nCreate a project or pick up where you left off."),
                                "font-size: 10.5pt; color: #c4cad4; background: transparent;"));
    heroLayout->addStretch();
    auto* heroButtons = new QHBoxLayout;
    heroButtons->setSpacing(10);
    auto* heroCreate = new QPushButton(theme::icon(QStringLiteral("plus")), tr("New Project"));
    heroCreate->setProperty("accent", true);
    auto* heroOpen = new QPushButton(theme::icon(QStringLiteral("open")), tr("Open Project…"));
    heroOpen->setStyleSheet(QStringLiteral("QPushButton { background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.18); color: #ffffff; }"
                                           "QPushButton:hover { background: rgba(255,255,255,0.18); }"));
    heroButtons->addWidget(heroCreate);
    heroButtons->addWidget(heroOpen);
    heroButtons->addStretch();
    heroLayout->addLayout(heroButtons);
    layout->addWidget(hero_);

    // Section header
    auto* header = new QHBoxLayout;
    auto* titleBox = new QVBoxLayout;
    titleBox->setSpacing(0);
    titleBox->addWidget(label(tr("Recent projects"), "font-size: 13pt; font-weight: 650; color: #eef0f3;"));
    countLabel_ = new QLabel;
    countLabel_->setProperty("muted", true);
    titleBox->addWidget(countLabel_);
    header->addLayout(titleBox);
    header->addStretch();
    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Search projects"));
    search->addAction(theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    search->setClearButtonEnabled(true);
    search->setFixedWidth(260);
    search->setMinimumHeight(32);
    auto* sort = new QComboBox;
    sort->addItems({tr("Last opened"), tr("Name")});
    sort->setMinimumHeight(32);
    header->addWidget(search);
    header->addWidget(sort);
    layout->addLayout(header);

    model_ = new ProjectListModel(app_.recentProjects(), this);
    filter_ = new ProjectFilterModel(this);
    filter_->setSourceModel(model_);
    filter_->setSort(ProjectFilterModel::Sort::Recent);

    list_ = new QListView;
    list_->setObjectName(QStringLiteral("projectList"));
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
    list_->setFrameShape(QFrame::NoFrame);
    list_->setStyleSheet(QStringLiteral("QListView { background: transparent; } QListView::item { background: transparent; }"));
    list_->viewport()->setAttribute(Qt::WA_Hover);

    // Empty state
    auto* empty = new QWidget;
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch();
    emptyLayout->addWidget(new EmptyIllustration, 0, Qt::AlignHCenter);
    emptyLayout->addSpacing(6);
    auto* emptyTitle = label(tr("No projects yet"), "font-size: 14pt; font-weight: 650; color: #eef0f3;");
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto* emptyText = new QLabel(tr("Start from a template – an empty world, a basic scene or a PBR material showcase."));
    emptyText->setProperty("muted", true);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyText);
    emptyLayout->addStretch();

    details_ = new ProjectDetailsPanel;
    details_->hide();
    auto* browser = new QWidget;
    auto* browserLayout = new QHBoxLayout(browser);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    browserLayout->setSpacing(14);
    browserLayout->addWidget(list_, 1);
    browserLayout->addWidget(details_, 0, Qt::AlignTop);

    projectStack_ = new QStackedWidget;
    projectStack_->addWidget(browser);
    projectStack_->addWidget(empty);
    layout->addWidget(projectStack_, 1);

    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) { filter_->setSearch(text); });
    connect(sort, &QComboBox::currentIndexChanged, this, [this](int i) {
        filter_->setSort(i == 0 ? ProjectFilterModel::Sort::Recent : ProjectFilterModel::Sort::Name);
    });
    connect(heroCreate, &QPushButton::clicked, this, &HubWindow::newProject);
    connect(heroOpen, &QPushButton::clicked, this, &HubWindow::openFromDisk);
    connect(list_, &QListView::activated, this, [this](const QModelIndex& index) {
        if (!index.data(ProjectListModel::ExistsRole).toBool()) {
            QMessageBox::warning(this, tr("Project missing"),
                                 tr("The project was moved or deleted:\n%1").arg(index.data(ProjectListModel::DescriptorRole).toString()));
            return;
        }
        emit openProjectRequested(index.data(ProjectListModel::DescriptorRole).toString());
    });
    connect(list_, &QListView::customContextMenuRequested, this, &HubWindow::showContextMenu);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) { details_->showProject(current); });
    connect(details_, &ProjectDetailsPanel::openRequested, this, &HubWindow::openProjectRequested);
    connect(details_, &ProjectDetailsPanel::settingsRequested, this, &HubWindow::openProjectSettings);
    connect(details_, &ProjectDetailsPanel::revealRequested, this,
            [](const QString& root) { QDesktopServices::openUrl(QUrl::fromLocalFile(root)); });
    connect(details_, &ProjectDetailsPanel::removeRequested, this, &HubWindow::removeFromList);
    return page;
}

QWidget* HubWindow::buildAboutPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 20);
    layout->setSpacing(12);
    layout->addWidget(label(tr("About Lumora"), "font-size: 18pt; font-weight: 700; color: #f0f2f5;"));
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
    aboutGpus_->setStyleSheet(QStringLiteral("font-family: monospace; background: #15171c; border: 1px solid #262a33;"
                                             "border-radius: 10px; padding: 14px; color: #c9ced6;"));
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
    countLabel_->setText(count == 0 ? tr("Nothing here yet") : count == 1 ? tr("1 project") : tr("%1 projects").arg(count));
    heroTitle_->setText(count == 0 ? tr("Welcome to Lumora") : tr("Welcome back"));
}

void HubWindow::refresh() {
    const QModelIndex current = list_->currentIndex();
    const QString selected = current.isValid() ? current.data(ProjectListModel::DescriptorRole).toString() : QString();
    model_->reload();
    updateEmptyState();
    if (!selected.isEmpty()) selectDescriptor(selected);
    if (!list_->currentIndex().isValid() && filter_->rowCount() > 0) {
        // Preselect the most recent project: its details show at once and Enter opens it.
        const QModelIndex first = filter_->index(0, 0);
        list_->setCurrentIndex(first);
        details_->showProject(first);
    }
    if (!list_->currentIndex().isValid()) details_->hide();
}

void HubWindow::selectDescriptor(const QString& descriptor) {
    const QModelIndex source = model_->indexOf(descriptor);
    const QModelIndex proxy = filter_->mapFromSource(source);
    if (!proxy.isValid()) return;
    list_->setCurrentIndex(proxy);
    list_->scrollTo(proxy);
    details_->showProject(proxy);
}

void HubWindow::openProjectSettings(const QString& descriptor) {
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
}

void HubWindow::removeFromList(const QString& descriptor) {
    // Only the list entry is removed; project files are never deleted from the Hub.
    app_.recentProjects().remove(toPath(descriptor));
    app_.saveRecentProjects();
    list_->setCurrentIndex({});
    refresh();
}

bool HubWindow::addExistingProject(const QString& path) {
    auto project = Project::load(toPath(path));
    if (!project) {
        QMessageBox::warning(this, tr("Not a Lumora project"), QString::fromStdString(project.error().message));
        return false;
    }
    app_.recentProjects().touch(project->descriptorPath(), project->name());
    app_.saveRecentProjects();
    refresh();
    selectDescriptor(toQString(project->descriptorPath()));
    return true;
}

void HubWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void HubWindow::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) addExistingProject(url.toLocalFile());
    }
    event->acceptProposedAction();
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
        openProjectSettings(descriptor);
    } else if (chosen == remove) {
        removeFromList(descriptor);
    }
}

} // namespace lumora::app
