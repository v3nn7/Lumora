#pragma once

#include "common/ModuleExport.hpp"

#include <QWidget>

class QFileSystemModel;
class QHBoxLayout;
class QLabel;
class QListView;
class QSortFilterProxyModel;
class QToolButton;
class QTreeView;
class QIcon;

namespace lumora::editor {

/// The project's files: a folder tree on the left, the open folder's contents on the right, a
/// clickable path above. Rooted at the project folder (editor state in Saved/ is hidden), so
/// Content/ and Scripts/ are both here.
///
/// Double-click opens: folders, scenes (in the editor), models and prefabs (placed in the scene),
/// HDR images (as the sky), scripts and text files (in the code editor). The context menu and the
/// New button create folders, scenes, scripts and prefabs, import files, and rename, duplicate or
/// delete (to the system trash).
class LUMORA_EDITOR_API ContentBrowserPanel final : public QWidget {
    Q_OBJECT
public:
    explicit ContentBrowserPanel(const QString& projectRoot, QWidget* parent = nullptr);

    [[nodiscard]] QString currentFolder() const { return current_; }
    /// Previews and type icons for the files.
    void setThumbnails(class ThumbnailCache* thumbnails);
    /// Icon shown for a file (tests).
    [[nodiscard]] QIcon iconOf(const QString& path) const;
    void navigate(const QString& dir);

signals:
    void openSceneRequested(const QString& path);
    /// A model or prefab inside the project should be placed in the scene.
    void addModelRequested(const QString& path);
    /// Files from outside (dropped or chosen with Import) to bring into `targetDir`.
    void importRequested(const QStringList& files, const QString& targetDir);
    void newSceneRequested(const QString& path);
    void newBehaviourRequested();
    void prefabFromSelectionRequested(const QString& folder);
    void openCodeRequested(const QString& path);
    /// A file was selected or opened (materials show in the Inspector).
    void assetSelected(const QString& path);
    void applyMaterialRequested(const QString& path);
    /// A timeline (.ltimeline) should open in the Timeline panel.
    void openTimelineRequested(const QString& path);
    /// A visual script (.lgraph) should open in the Visual Script panel.
    void openGraphRequested(const QString& path);
    /// A material graph (.lmgraph) to edit.
    void openMaterialGraphRequested(const QString& path);

public:
    /// Writes a default material into the open folder; returns its path (empty on failure).
    QString createMaterial(QString name);
    /// Creates a material graph (a Material Output only) in the current folder and opens it;
    /// empty if it exists already.
    QString createMaterialGraph(QString name);
    /// Starter Lua behaviour / visual script in the open folder; their path (empty on failure).
    QString createLuaScript(QString name);
    QString createVisualScript(QString name);
    /// Moves project files into a folder (dragging them onto it).
    void moveInto(const QStringList& files, const QString& folder);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showContextMenu(const QPoint& pos);
    void populateNewMenu(class QMenu& menu);
    void activate(const QString& path);
    void newFolder();
    void newScene();
    void newMaterial();
    void newMaterialGraph();
    void newTimeline();
    void newLuaScript();
    void newVisualScript();
    void newAnimatorController();
    void importFiles();
    void duplicate(const QString& path);
    void remove(const QStringList& paths);
    void rebuildBreadcrumbs();
    [[nodiscard]] QStringList selectedPaths() const;

    QString root_;
    QString current_;
    QFileSystemModel* model_ = nullptr;
    QFileSystemModel* folders_ = nullptr;
    QSortFilterProxyModel* folderFilter_ = nullptr;
    QTreeView* tree_ = nullptr;
    QListView* view_ = nullptr;
    QHBoxLayout* crumbs_ = nullptr;
    QToolButton* up_ = nullptr;
};

} // namespace lumora::editor
