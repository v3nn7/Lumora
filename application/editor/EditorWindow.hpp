#pragma once

#include "editor/EditorContext.hpp"
#include "assets/AssetLibrary.hpp"
#include "project/Project.hpp"

#include <QMainWindow>

#include <memory>
#include <optional>
#include <string>

class QAction;
class QLabel;
class QFrame;
class QStackedWidget;

namespace lumora {
class Scene;
class Simulation;
}

namespace lumora::app {
class AppContext;
class VulkanViewport;
}

namespace lumora::editor {

class EditorViewportClient;
class ProfilerPanel;

/// Lumora Editor main window: Vulkan viewport, dockable panels, play mode, scene I/O.
class EditorWindow final : public QMainWindow {
    Q_OBJECT
public:
    EditorWindow(app::AppContext& app, Project project, QWidget* parent = nullptr);
    ~EditorWindow() override;

    /// Loads a scene file into the editor (replacing the current one, after prompting to save).
    bool openScene(const QString& path, bool prompt = true);
    bool saveScene();
    void play();
    void togglePause();
    void stop();
    [[nodiscard]] bool isPlaying() const noexcept { return simulation_ != nullptr; }
    [[nodiscard]] bool isPaused() const noexcept { return paused_; }
    [[nodiscard]] const Project& project() const noexcept { return project_; }
    [[nodiscard]] EditorContext& context() noexcept { return ctx_; }
    [[nodiscard]] Scene* editScene() const noexcept { return editScene_.get(); }
    [[nodiscard]] const QString& scenePath() const noexcept { return scenePath_; }
    [[nodiscard]] AssetLibrary& assets() noexcept { return assets_; }
    /// Instantiates a model that lives inside the project into the scene (one undo step).
    entt::entity addModelToScene(const QString& absolutePath);
    /// Copies external model files into the project and adds them to the scene.
    void importModels(const QStringList& files, const QString& targetDir);

signals:
    void closeProjectRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildActions();
    void buildMenus();
    void buildToolbar();
    void buildCentral();
    void buildDocks();
    void buildStatusBar();
    void restoreLayout();
    void saveLayout();
    void resetLayout();

    void newScene();
    void openSceneDialog();
    void importModelDialog();
    [[nodiscard]] std::optional<std::string> pickMeshAsset(QWidget* parent);
    bool saveSceneAs();
    bool maybeSave();
    void saveThumbnail();
    void setEditScene(std::unique_ptr<Scene> scene, const QString& path);

    void updatePlayState();
    void updateTitle();
    bool handleShortcut(int key, Qt::KeyboardModifiers modifiers);
    void showControls();
    void onRenderFailed(const QString& message);

    app::AppContext& app_;
    Project project_;
    AssetLibrary assets_;  // project assets (imported models); referenced by the viewport client
    std::unique_ptr<Scene> editScene_;
    std::unique_ptr<Scene> playScene_;
    std::unique_ptr<Simulation> simulation_;
    bool paused_ = false;
    QString scenePath_;
    EditorContext ctx_;
    std::unique_ptr<EditorViewportClient> client_;

    app::VulkanViewport* viewport_ = nullptr;
    QFrame* viewportFrame_ = nullptr;
    QStackedWidget* viewportStack_ = nullptr;
    QLabel* viewportError_ = nullptr;
    ProfilerPanel* profiler_ = nullptr;
    QByteArray defaultLayout_;

    QAction* newScene_ = nullptr;
    QAction* openScene_ = nullptr;
    QAction* saveScene_ = nullptr;
    QAction* saveSceneAs_ = nullptr;
    QAction* importModel_ = nullptr;
    QAction* projectSettings_ = nullptr;
    QAction* closeProject_ = nullptr;
    QAction* exit_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* duplicate_ = nullptr;
    QAction* delete_ = nullptr;
    QAction* frame_ = nullptr;
    QAction* play_ = nullptr;
    QAction* pause_ = nullptr;
    QAction* stop_ = nullptr;
    QAction* pilot_ = nullptr;
    QAction* grid_ = nullptr;
    QAction* vsync_ = nullptr;
    QAction* resetLayout_ = nullptr;
    QAction* moveTool_ = nullptr;
    QAction* rotateTool_ = nullptr;
    QAction* scaleTool_ = nullptr;

    QLabel* toolbarScene_ = nullptr;
    QLabel* modeChip_ = nullptr;
    QLabel* statusScene_ = nullptr;
    QLabel* statusCamera_ = nullptr;
    QLabel* statusFps_ = nullptr;
    QLabel* statusGpu_ = nullptr;
};

} // namespace lumora::editor
