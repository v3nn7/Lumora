#include "editor/ContentBrowserPanel.hpp"

#include "common/Theme.hpp"

#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDir>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {
bool isModel(const QFileInfo& info) {
    return info.suffix().compare(QLatin1String("glb"), Qt::CaseInsensitive) == 0 ||
           info.suffix().compare(QLatin1String("gltf"), Qt::CaseInsensitive) == 0;
}
/// Equirectangular HDR environment: dropping or opening one makes it the scene's sky.
bool isHdr(const QFileInfo& info) { return info.suffix().compare(QLatin1String("hdr"), Qt::CaseInsensitive) == 0; }

class AssetIconProvider final : public QFileIconProvider {
public:
    QIcon icon(const QFileInfo& info) const override {
        if (info.isDir()) return app::theme::icon(QStringLiteral("folder"));
        if (info.suffix() == QLatin1String("lscene")) return app::theme::icon(QStringLiteral("scene"));
        if (info.suffix().compare(QLatin1String("glb"), Qt::CaseInsensitive) == 0 ||
            info.suffix().compare(QLatin1String("gltf"), Qt::CaseInsensitive) == 0) {
            return app::theme::icon(QStringLiteral("cube"));
        }
        return app::theme::icon(QStringLiteral("file"));
    }
    using QFileIconProvider::icon;
};
} // namespace

ContentBrowserPanel::ContentBrowserPanel(const QString& contentRoot, QWidget* parent)
    : QWidget(parent), root_(QDir(contentRoot).absolutePath()) {
    QDir().mkpath(root_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* bar = new QHBoxLayout;
    up_ = new QToolButton;
    up_->setIcon(app::theme::icon(QStringLiteral("back")));
    up_->setToolTip(tr("Up one folder"));
    path_ = new QLabel;
    path_->setProperty("muted", true);
    auto* newFolder = new QToolButton;
    newFolder->setIcon(app::theme::icon(QStringLiteral("plus")));
    newFolder->setToolTip(tr("New folder"));
    auto* reveal = new QToolButton;
    reveal->setIcon(app::theme::icon(QStringLiteral("open")));
    reveal->setToolTip(tr("Show in file manager"));
    bar->addWidget(up_);
    bar->addWidget(path_, 1);
    bar->addWidget(newFolder);
    bar->addWidget(reveal);
    layout->addLayout(bar);

    model_ = new QFileSystemModel(this);
    model_->setIconProvider(new AssetIconProvider);
    model_->setReadOnly(false);
    model_->setRootPath(root_);
    model_->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);

    view_ = new QListView;
    view_->setModel(model_);
    view_->setViewMode(QListView::IconMode);
    view_->setIconSize(QSize(48, 48));
    view_->setGridSize(QSize(104, 92));
    view_->setResizeMode(QListView::Adjust);
    view_->setMovement(QListView::Static);
    view_->setWordWrap(true);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(view_, 1);

    connect(view_, &QListView::activated, this, [this](const QModelIndex& index) {
        const QFileInfo info = model_->fileInfo(index);
        if (info.isDir()) {
            navigate(info.absoluteFilePath());
        } else if (info.suffix() == QLatin1String("lscene")) {
            emit openSceneRequested(info.absoluteFilePath());
        } else if (isModel(info)) {
            emit addModelRequested(info.absoluteFilePath());
        } else if (isHdr(info)) {
            emit importRequested({info.absoluteFilePath()}, current_);
        } else {
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
        }
    });
    connect(up_, &QToolButton::clicked, this, [this] { navigate(QFileInfo(current_).absolutePath()); });
    connect(newFolder, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Folder name"), QLineEdit::Normal,
                                                   tr("New Folder"), &ok);
        if (ok && !name.trimmed().isEmpty() && !QDir(current_).mkdir(name.trimmed())) {
            QMessageBox::warning(this, tr("New Folder"), tr("Could not create the folder."));
        }
    });
    connect(reveal, &QToolButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(current_)); });
    connect(view_, &QListView::customContextMenuRequested, this, &ContentBrowserPanel::showContextMenu);
    // Accept external model files dropped from the desktop / file manager.
    view_->setAcceptDrops(true);
    view_->viewport()->setAcceptDrops(true);
    view_->viewport()->installEventFilter(this);
    navigate(root_);
}

void ContentBrowserPanel::navigate(const QString& dir) {
    QString target = QDir(dir).absolutePath();
    if (!target.startsWith(root_)) target = root_;  // never leave the project content
    current_ = target;
    view_->setRootIndex(model_->index(current_));
    const QString relative = QDir(QFileInfo(root_).absolutePath()).relativeFilePath(current_);
    path_->setText(relative.split(QLatin1Char('/')).join(QStringLiteral("  ›  ")));
    up_->setEnabled(current_ != root_);
}

bool ContentBrowserPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* drag = static_cast<QDropEvent*>(event);
            if (drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            QStringList files;
            for (const QUrl& url : drop->mimeData()->urls()) {
                const QFileInfo dropped(url.toLocalFile());
                if (url.isLocalFile() && (isModel(dropped) || isHdr(dropped))) files << url.toLocalFile();
            }
            if (!files.isEmpty()) emit importRequested(files, current_);
            drop->acceptProposedAction();
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
        if (info.suffix() == QLatin1String("lscene")) {
            menu.addAction(app::theme::icon(QStringLiteral("scene")), tr("Open Scene"),
                           [this, info] { emit openSceneRequested(info.absoluteFilePath()); });
        }
        if (isHdr(info)) {
            menu.addAction(app::theme::icon(QStringLiteral("sun")), tr("Use as Sky"),
                           [this, info] { emit importRequested({info.absoluteFilePath()}, current_); });
        }
        if (isModel(info)) {
            menu.addAction(app::theme::icon(QStringLiteral("cube")), tr("Add to Scene"),
                           [this, info] { emit addModelRequested(info.absoluteFilePath()); });
        }
        menu.addAction(tr("Rename"), [this, index] { view_->edit(index); });
    }
    menu.addAction(app::theme::icon(QStringLiteral("open")), tr("Show in File Manager"),
                   [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(current_)); });
    menu.exec(view_->viewport()->mapToGlobal(pos));
}

} // namespace lumora::editor
