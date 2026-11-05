#pragma once

#include <QWidget>

class QFileSystemModel;
class QLabel;
class QListView;
class QToolButton;

namespace lumora::editor {

/// File browser rooted at the project's Content directory.
class ContentBrowserPanel final : public QWidget {
    Q_OBJECT
public:
    ContentBrowserPanel(const QString& contentRoot, QWidget* parent = nullptr);

signals:
    void openSceneRequested(const QString& path);
    /// A model inside the project should be placed in the scene.
    void addModelRequested(const QString& path);
    /// External model files were dropped onto the browser (to be imported into `targetDir`).
    void importRequested(const QStringList& files, const QString& targetDir);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void navigate(const QString& dir);
    void showContextMenu(const QPoint& pos);

    QString root_;
    QString current_;
    QFileSystemModel* model_ = nullptr;
    QListView* view_ = nullptr;
    QLabel* path_ = nullptr;
    QToolButton* up_ = nullptr;
};

} // namespace lumora::editor
