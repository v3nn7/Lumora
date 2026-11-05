#pragma once

#include <QWidget>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListView;
class QStackedWidget;

namespace lumora::app {

class AppContext;
class ProjectListModel;
class ProjectFilterModel;

/// Lumora Hub: recent projects, creation from templates, project settings, engine info.
class HubWindow final : public QWidget {
    Q_OBJECT
public:
    explicit HubWindow(AppContext& app, QWidget* parent = nullptr);

    void refresh();

signals:
    void openProjectRequested(const QString& descriptorPath);

private:
    QWidget* buildSidebar();
    QWidget* buildProjectsPage();
    QWidget* buildAboutPage();
    void newProject();
    void openFromDisk();
    void showContextMenu(const QPoint& pos);
    void updateAbout();
    void updateEmptyState();

    AppContext& app_;
    ProjectListModel* model_ = nullptr;
    ProjectFilterModel* filter_ = nullptr;
    QListView* list_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QStackedWidget* projectStack_ = nullptr;
    QLabel* aboutGpus_ = nullptr;
    QLabel* countLabel_ = nullptr;
    bool aboutFilled_ = false;
};

} // namespace lumora::app
