#pragma once

#include <QWidget>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListView;
class QStackedWidget;

namespace lumora::app {

class AppContext;
class HeroBanner;
class ProjectDetailsPanel;
class ProjectListModel;
class ProjectFilterModel;

/// Lumora Hub: recent projects, creation from templates, project settings, engine info.
class HubWindow final : public QWidget {
    Q_OBJECT
public:
    explicit HubWindow(AppContext& app, QWidget* parent = nullptr);

    void refresh();
    /// Adds an existing project (folder or .lumora file) to the recent list and selects it.
    /// Used for files dropped onto the Hub.
    bool addExistingProject(const QString& path);

signals:
    void openProjectRequested(const QString& descriptorPath);

protected:
    void showEvent(QShowEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QWidget* buildSidebar();
    QWidget* buildProjectsPage();
    QWidget* buildAboutPage();
    void newProject();
    void openFromDisk();
    void showContextMenu(const QPoint& pos);
    void updateAbout();
    void updateEmptyState();
    void renderHero();
    void openProjectSettings(const QString& descriptor);
    void removeFromList(const QString& descriptor);
    void selectDescriptor(const QString& descriptor);

    AppContext& app_;
    ProjectListModel* model_ = nullptr;
    ProjectFilterModel* filter_ = nullptr;
    QListView* list_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QStackedWidget* projectStack_ = nullptr;
    HeroBanner* hero_ = nullptr;
    ProjectDetailsPanel* details_ = nullptr;
    QLabel* heroTitle_ = nullptr;
    QLabel* aboutGpus_ = nullptr;
    QLabel* countLabel_ = nullptr;
    bool aboutFilled_ = false;
    bool heroRendered_ = false;
};

} // namespace lumora::app
