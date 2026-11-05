#pragma once

#include "common/ModuleExport.hpp"

#include <QWidget>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListView;
class QStackedWidget;

namespace lumora::app {

class AppContext;
class EditorsPage;
class HeroBanner;
class ProjectDetailsPanel;
class ProjectListModel;
class ProjectFilterModel;

/// Lumora Hub: recent projects, creation from templates, project settings, engine info.
class LUMORA_HUB_API HubWindow final : public QWidget {
    Q_OBJECT
public:
    explicit HubWindow(AppContext& app, QWidget* parent = nullptr);

    void refresh();
    /// Adds an existing project (folder or .lumora file) to the recent list and selects it.
    /// Used for files dropped onto the Hub.
    bool addExistingProject(const QString& path);
    /// Switches to the Editors page, optionally with a note (e.g. a project needs a version).
    void showEditorsPage(const QString& message = {});

signals:
    void openProjectRequested(const QString& descriptorPath);
    void joinSessionRequested();

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
    EditorsPage* editorsPage_ = nullptr;
    QButtonGroup* nav_ = nullptr;
};

} // namespace lumora::app
