#include "editor/ContentBrowserPanel.hpp"

#include "common/Theme.hpp"
#include "assets/Material.hpp"
#include "common/ProjectUi.hpp"
#include "editor/Thumbnails.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {

bool hasSuffix(const QFileInfo& info, std::initializer_list<const char*> suffixes) {
    for (const char* s : suffixes) {
        if (info.suffix().compare(QLatin1String(s), Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}
bool isModel(const QFileInfo& info) { return hasSuffix(info, {"glb", "gltf"}); }
bool isPrefab(const QFileInfo& info) { return hasSuffix(info, {"lprefab"}); }
/// Equirectangular HDR environment: dropping or opening one makes it the scene's sky.
bool isHdr(const QFileInfo& info) { return hasSuffix(info, {"hdr"}); }
bool isMaterial(const QFileInfo& info) { return hasSuffix(info, {"lmat"}); }
bool isCode(const QFileInfo& info) {
    return hasSuffix(info, {"cpp", "hpp", "h", "cc", "cxx", "inl", "txt", "md", "json", "lumora", "lscene", "lprefab"});
}
/// Files the browser can bring into the project.
bool isImportable(const QFileInfo& info) {
    return isModel(info) || isPrefab(info) || isHdr(info) || isMaterial(info) ||
           hasSuffix(info, {"png", "jpg", "jpeg", "tga", "bmp", "wav", "flac", "mp3", "cpp", "hpp", "h"});
}

class AssetIconProvider final : public QFileIconProvider {
public:
    QIcon icon(const QFileInfo& info) const override {
        if (info.isDir()) return app::theme::icon(QStringLiteral("folder"));
        if (info.suffix() == QLatin1String("lscene")) return app::theme::icon(QStringLiteral("scene"));
        if (isModel(info)) return app::theme::icon(QStringLiteral("cube"));
        if (isPrefab(info)) return app::theme::icon(QStringLiteral("entity"));
        if (isHdr(info)) return app::theme::icon(QStringLiteral("sun"));
        if (isMaterial(info)) return app::theme::icon(QStringLiteral("sphere"));
        if (hasSuffix(info, {"cpp", "hpp", "h"})) return app::theme::icon(QStringLiteral("console"));
        return app::theme::icon(QStringLiteral("file"));
    }
    using QFileIconProvider::icon;
};

/// The browser's files with previews (or type icons) from a ThumbnailCache.
class AssetFileModel final : public QFileSystemModel {
public:
    using QFileSystemModel::QFileSystemModel;
    ThumbnailCache* thumbnails = nullptr;
    QVariant data(const QModelIndex& index, int role) const override {
        if (role == Qt::DecorationRole && thumbnails != nullptr && index.column() == 0 && !isDir(index)) {
            return thumbnails->icon(fileInfo(index));
        }
        return QFileSystemModel::data(index, role);
    }
};

/// Hides the editor's own state (Saved/) at the project's top level.
class HideSaved final : public QSortFilterProxyModel {
public:
    explicit HideSaved(QString root, QObject* parent) : QSortFilterProxyModel(parent), root_(std::move(root)) {}

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        auto* fs = static_cast<QFileSystemModel*>(sourceModel());
        const QModelIndex index = fs->index(row, 0, parent);
        const QFileInfo info = fs->fileInfo(index);
        if (info.absolutePath() == root_ && (info.fileName() == QLatin1String("Saved") || info.fileName().startsWith('.'))) {
            return false;
        }
        return true;
    }

private:
    QString root_;
};

} // namespace

ContentBrowserPanel::ContentBrowserPanel(const QString& projectRoot, QWidget* parent)
    : QWidget(parent), root_(QDir(projectRoot).absolutePath()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // Toolbar: up, clickable path, New menu, Import, reveal.
    auto* bar = new QHBoxLayout;
    up_ = new QToolButton;
    up_->setIcon(app::theme::icon(QStringLiteral("back")));
    up_->setToolTip(tr("Up one folder"));
    crumbs_ = new QHBoxLayout;
    crumbs_->setSpacing(0);
    auto* newButton = new QToolButton;
    newButton->setIcon(app::theme::icon(QStringLiteral("plus")));
    newButton->setText(tr("New"));
    newButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    newButton->setPopupMode(QToolButton::InstantPopup);
    auto* newMenu = new QMenu(newButton);
    connect(newMenu, &QMenu::aboutToShow, this, [this, newMenu] {
        newMenu->clear();
        populateNewMenu(*newMenu);
    });
    newButton->setMenu(newMenu);
    auto* import = new QToolButton;
    import->setIcon(app::theme::icon(QStringLiteral("open")));
    import->setText(tr("Import"));
    import->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    import->setToolTip(tr("Copy models, textures, sounds or HDR skies into this folder"));
    auto* reveal = new QToolButton;
    reveal->setIcon(app::theme::icon(QStringLiteral("folder")));
    reveal->setToolTip(tr("Show in file manager"));
    bar->addWidget(up_);
    bar->addLayout(crumbs_, 1);
    bar->addWidget(newButton);
    bar->addWidget(import);
    bar->addWidget(reveal);
    layout->addLayout(bar);

    // Folder tree (left) and the folder's contents (right).
    folders_ = new QFileSystemModel(this);
    folders_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    folders_->setRootPath(root_);
    folderFilter_ = new HideSaved(root_, this);
    folderFilter_->setSourceModel(folders_);
    tree_ = new QTreeView;
    tree_->setModel(folderFilter_);
    tree_->setRootIndex(folderFilter_->mapFromSource(folders_->index(root_)));
    for (int c = 1; c < folders_->columnCount(); ++c) tree_->hideColumn(c);
    tree_->header()->hide();
    tree_->setMinimumWidth(140);

    model_ = new AssetFileModel(this);
    model_->setIconProvider(new AssetIconProvider);
    model_->setReadOnly(false);  // rename in place
    model_->setRootPath(root_);
    model_->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    view_ = new QListView;
    view_->setModel(model_);
    view_->setViewMode(QListView::IconMode);
    view_->setIconSize(QSize(64, 64));
    view_->setGridSize(QSize(110, 108));
    view_->setResizeMode(QListView::Adjust);
    view_->setMovement(QListView::Static);
    view_->setWordWrap(true);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* split = new QSplitter;
    split->addWidget(tree_);
    split->addWidget(view_);
    split->setStretchFactor(1, 1);
    split->setSizes({170, 500});
    layout->addWidget(split, 1);

    connect(view_, &QListView::activated, this,
            [this](const QModelIndex& index) { activate(model_->fileInfo(index).absoluteFilePath()); });
    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        navigate(folders_->filePath(folderFilter_->mapToSource(index)));
    });
    connect(up_, &QToolButton::clicked, this, [this] { navigate(QFileInfo(current_).absolutePath()); });
    connect(import, &QToolButton::clicked, this, &ContentBrowserPanel::importFiles);
    connect(reveal, &QToolButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(current_)); });
    connect(view_, &QListView::customContextMenuRequested, this, &ContentBrowserPanel::showContextMenu);
    // Files drag out (onto the viewport or the Hierarchy) and into folders.
    view_->setDragEnabled(true);
    view_->setDragDropMode(QAbstractItemView::DragDrop);
    view_->setDefaultDropAction(Qt::CopyAction);
    view_->setAcceptDrops(true);
    view_->viewport()->setAcceptDrops(true);
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        if (index.isValid()) emit assetSelected(model_->filePath(index));
    });
    view_->viewport()->installEventFilter(this);
    view_->installEventFilter(this);  // Delete / F2 / Backspace
    navigate(QDir(root_).exists(QStringLiteral("Content")) ? QDir(root_).filePath(QStringLiteral("Content")) : root_);
}

QIcon ContentBrowserPanel::iconOf(const QString& path) const {
    return qvariant_cast<QIcon>(model_->data(model_->index(path), Qt::DecorationRole));
}

void ContentBrowserPanel::setThumbnails(ThumbnailCache* thumbnails) {
    static_cast<AssetFileModel*>(model_)->thumbnails = thumbnails;
    connect(thumbnails, &ThumbnailCache::previewReady, this, [this](const QString& path) {
        const QModelIndex index = model_->index(path);
        if (index.isValid()) emit model_->dataChanged(index, index, {Qt::DecorationRole});
    });
    view_->viewport()->update();
}

void ContentBrowserPanel::navigate(const QString& dir) {
    QString target = QDir(dir).absolutePath();
    if (!target.startsWith(root_) || !QFileInfo(target).isDir()) target = root_;  // never leave the project
    current_ = target;
    view_->setRootIndex(model_->index(current_));
    up_->setEnabled(current_ != root_);
    const QModelIndex treeIndex = folderFilter_->mapFromSource(folders_->index(current_));
    tree_->setCurrentIndex(treeIndex);
    tree_->expand(treeIndex);
    rebuildBreadcrumbs();
}

void ContentBrowserPanel::rebuildBreadcrumbs() {
    while (QLayoutItem* item = crumbs_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    // Project › Content › Models: each part jumps there.
    QString path = root_;
    QStringList parts{QFileInfo(root_).fileName()};
    const QString relative = QDir(root_).relativeFilePath(current_);
    if (relative != QLatin1String(".")) parts += relative.split(QLatin1Char('/'));
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            path = QDir(path).filePath(parts[i]);
            auto* sep = new QLabel(QStringLiteral("›"));
            sep->setProperty("muted", true);
            crumbs_->addWidget(sep);
        }
        auto* crumb = new QToolButton;
        crumb->setText(parts[i]);
        crumb->setAutoRaise(true);
        connect(crumb, &QToolButton::clicked, this, [this, path] { navigate(path); });
        crumbs_->addWidget(crumb);
    }
    crumbs_->addStretch(1);
}

void ContentBrowserPanel::activate(const QString& path) {
    const QFileInfo info(path);
    if (info.isDir()) {
        navigate(path);
    } else if (info.suffix() == QLatin1String("lscene")) {
        emit openSceneRequested(path);
    } else if (isModel(info) || isPrefab(info)) {
        emit addModelRequested(path);
    } else if (isHdr(info)) {
        emit importRequested({path}, current_);
    } else if (isMaterial(info)) {
        emit assetSelected(path);
    } else if (isCode(info)) {
        emit openCodeRequested(path);
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void ContentBrowserPanel::populateNewMenu(QMenu& menu) {
    menu.addAction(app::theme::icon(QStringLiteral("folder")), tr("Folder…"), this, &ContentBrowserPanel::newFolder);
    menu.addAction(app::theme::icon(QStringLiteral("scene")), tr("Scene…"), this, &ContentBrowserPanel::newScene);
    menu.addAction(app::theme::icon(QStringLiteral("sphere")), tr("Material…"), this, &ContentBrowserPanel::newMaterial);
    menu.addAction(app::theme::icon(QStringLiteral("console")), tr("C++ Behaviour…"), this,
                   [this] { emit newBehaviourRequested(); });
    menu.addAction(app::theme::icon(QStringLiteral("entity")), tr("Prefab from Selection"), this,
                   [this] { emit prefabFromSelectionRequested(current_); });
}

void ContentBrowserPanel::newFolder() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Folder name"), QLineEdit::Normal,
                                               tr("New Folder"), &ok)
                             .trimmed();
    if (ok && !name.isEmpty() && !QDir(current_).mkdir(name)) {
        QMessageBox::warning(this, tr("New Folder"), tr("Could not create the folder (does it exist already?)."));
    }
}

void ContentBrowserPanel::newScene() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Scene"), tr("Scene name"), QLineEdit::Normal, tr("Level"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!name.endsWith(QLatin1String(".lscene"))) name += QStringLiteral(".lscene");
    const QString path = QDir(current_).filePath(name);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("New Scene"), tr("%1 already exists.").arg(name));
        return;
    }
    emit newSceneRequested(path);
}

void ContentBrowserPanel::newMaterial() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Material"), tr("Material name"), QLineEdit::Normal,
                                         tr("Material"), &ok)
                       .trimmed();
    if (!ok || name.isEmpty()) return;
    const QString path = createMaterial(name);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("New Material"), tr("Could not create %1 (does it exist already?).").arg(name));
    }
}

QString ContentBrowserPanel::createMaterial(QString name) {
    if (!name.endsWith(QLatin1String(".lmat"))) name += QStringLiteral(".lmat");
    const QString path = QDir(current_).filePath(name);
    if (QFileInfo::exists(path) || !saveMaterial(MaterialAsset{}, app::toPath(path))) return {};
    emit assetSelected(path);
    return path;
}

void ContentBrowserPanel::moveInto(const QStringList& files, const QString& folder) {
    for (const QString& file : files) {
        const QFileInfo info(file);
        const QString target = QDir(folder).filePath(info.fileName());
        if (info.absoluteFilePath() == target || folder.startsWith(info.absoluteFilePath() + QLatin1Char('/'))) continue;
        if (QFileInfo::exists(target) || !QFile::rename(file, target)) {
            QMessageBox::warning(this, tr("Move"), tr("Could not move %1 into %2.").arg(info.fileName(), QFileInfo(folder).fileName()));
        }
    }
}

void ContentBrowserPanel::importFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Import into %1").arg(QDir(root_).relativeFilePath(current_)), QString(),
        tr("Assets (*.glb *.gltf *.png *.jpg *.jpeg *.tga *.bmp *.hdr *.wav *.flac *.mp3 *.lprefab);;All files (*)"));
    if (!files.isEmpty()) emit importRequested(files, current_);
}

void ContentBrowserPanel::duplicate(const QString& path) {
    const QFileInfo info(path);
    if (info.isDir()) return;
    QString copy;
    for (int n = 1; n < 1000; ++n) {
        copy = QDir(info.absolutePath()).filePath(QStringLiteral("%1 %2.%3").arg(info.completeBaseName()).arg(n + 1).arg(info.suffix()));
        if (!QFileInfo::exists(copy)) break;
    }
    if (!QFile::copy(path, copy)) QMessageBox::warning(this, tr("Duplicate"), tr("Could not copy %1.").arg(info.fileName()));
}

void ContentBrowserPanel::remove(const QStringList& paths) {
    if (paths.isEmpty()) return;
    const QString what = paths.size() == 1 ? QFileInfo(paths.front()).fileName() : tr("%n items", nullptr, static_cast<int>(paths.size()));
    if (QMessageBox::question(this, tr("Delete"), tr("Move %1 to the trash?").arg(what)) != QMessageBox::Yes) return;
    for (const QString& path : paths) {
        if (!QFile::moveToTrash(path)) {
            QMessageBox::warning(this, tr("Delete"), tr("Could not move %1 to the trash.").arg(QFileInfo(path).fileName()));
        }
    }
}

QStringList ContentBrowserPanel::selectedPaths() const {
    QStringList paths;
    for (const QModelIndex& index : view_->selectionModel()->selectedIndexes()) paths << model_->filePath(index);
    return paths;
}

bool ContentBrowserPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete) {
            remove(selectedPaths());
            return true;
        }
        if (key->key() == Qt::Key_Backspace && current_ != root_) {
            navigate(QFileInfo(current_).absolutePath());
            return true;
        }
    }
    if (watched == view_->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* drag = static_cast<QDropEvent*>(event);
            if (drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            // Onto a folder: there; anywhere else: the open folder.
            const QModelIndex target = view_->indexAt(drop->position().toPoint());
            const QString folder = target.isValid() && model_->isDir(target) ? model_->filePath(target) : current_;
            QStringList files;
            QStringList moved;
            for (const QUrl& url : drop->mimeData()->urls()) {
                if (!url.isLocalFile()) continue;
                const QFileInfo dropped(url.toLocalFile());
                if (dropped.absoluteFilePath().startsWith(root_ + QLatin1Char('/'))) {
                    moved << dropped.absoluteFilePath();  // already in the project: a move
                } else if (isImportable(dropped)) {
                    files << dropped.absoluteFilePath();
                }
            }
            if (!moved.isEmpty() && folder != current_) moveInto(moved, folder);
            if (!files.isEmpty()) emit importRequested(files, folder);
            // Copy, never Move: the dragging view would then try to remove the source rows.
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ContentBrowserPanel::showContextMenu(const QPoint& pos) {
    const QModelIndex index = view_->indexAt(pos);
    QMenu menu(this);
    if (index.isValid()) {
        const QFileInfo info = model_->fileInfo(index);
        const QString path = info.absoluteFilePath();
        if (info.isDir()) menu.addAction(app::theme::icon(QStringLiteral("folder")), tr("Open"), [this, path] { navigate(path); });
        if (info.suffix() == QLatin1String("lscene")) {
            menu.addAction(app::theme::icon(QStringLiteral("scene")), tr("Open Scene"), [this, path] { emit openSceneRequested(path); });
        }
        if (isModel(info) || isPrefab(info)) {
            menu.addAction(app::theme::icon(QStringLiteral("cube")), tr("Add to Scene"), [this, path] { emit addModelRequested(path); });
        }
        if (isHdr(info)) {
            menu.addAction(app::theme::icon(QStringLiteral("sun")), tr("Use as Sky"), [this, path] { emit importRequested({path}, current_); });
        }
        if (isCode(info)) {
            menu.addAction(app::theme::icon(QStringLiteral("console")), tr("Edit in Code Editor"), [this, path] { emit openCodeRequested(path); });
        }
        if (isMaterial(info)) {
            menu.addAction(app::theme::icon(QStringLiteral("sphere")), tr("Edit Material"), [this, path] { emit assetSelected(path); });
            menu.addAction(app::theme::icon(QStringLiteral("cube")), tr("Apply to Selected Object"),
                           [this, path] { emit applyMaterialRequested(path); });
        }
        menu.addSeparator();
        menu.addAction(tr("Rename"), [this, index] { view_->edit(index); });
        if (!info.isDir()) menu.addAction(tr("Duplicate"), [this, path] { duplicate(path); });
        menu.addAction(app::theme::icon(QStringLiteral("trash")), tr("Delete"), [this] { remove(selectedPaths()); });
        menu.addSeparator();
    }
    QMenu* create = menu.addMenu(app::theme::icon(QStringLiteral("plus")), tr("New"));
    populateNewMenu(*create);
    menu.addAction(app::theme::icon(QStringLiteral("open")), tr("Import…"), this, &ContentBrowserPanel::importFiles);
    menu.addAction(app::theme::icon(QStringLiteral("folder")), tr("Show in File Manager"),
                   [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(current_)); });
    menu.exec(view_->viewport()->mapToGlobal(pos));
}

} // namespace lumora::editor
