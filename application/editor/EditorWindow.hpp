#pragma once

#include "common/ModuleExport.hpp"

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
class ScriptModule;
class Simulation;
}

namespace lumora::app {
class AppContext;
class ScriptBuilder;
class VulkanViewport;
}

namespace lumora::collab {
class CollabClient;
class CollabPanel;
class CollabServer;
class CollabSession;
}

class QDockWidget;

namespace lumora::editor {

class EditorViewportClient;
class ProfilerPanel;

/// Lumora Editor main window: Vulkan viewport, dockable panels, play mode, scene I/O.
class LUMORA_EDITOR_API EditorWindow final : public QMainWindow {
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
    /// Asks to save unsaved changes and leaves play mode; false if the user cancelled.
    bool prepareToClose();

    // ---- Live collaboration ----------------------------------------------------------------
    /// Shows the host dialog, starts a session server for the current scene and joins it.
    void hostSession();
    struct HostOptions {
        quint16 port = 0;  // 0 = any free port
        QString name;
        QString password;
        QString publicAddress;  // shown to share (e.g. a playit.gg address)
    };
    /// Starts hosting without the dialog; the session begins once the local client has joined.
    Result<> startHosting(const HostOptions& options);
    /// Continues a session the Application already joined (the editor adopts the client).
    void attachSession(std::unique_ptr<collab::CollabClient> client, const nlohmann::json& scene, quint64 seq);
    void leaveSession(const QString& note = {});
    [[nodiscard]] bool inSession() const noexcept { return collabSession_ != nullptr; }
    [[nodiscard]] collab::CollabServer* sessionServer() const noexcept { return collabServer_.get(); }
    [[nodiscard]] collab::CollabClient* sessionClient() const noexcept { return collabClient_.get(); }

signals:
    void closeProjectRequested();
    void joinSessionRequested();

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
    [[nodiscard]] std::optional<std::string> pickTextureAsset(QWidget* parent);
    [[nodiscard]] std::optional<std::string> pickHdrAsset(QWidget* parent);
    [[nodiscard]] std::optional<std::string> pickAudioAsset(QWidget* parent);
    bool saveSceneAs();
    bool maybeSave();
    void saveThumbnail();
    void setEditScene(std::unique_ptr<Scene> scene, const QString& path);

    void updatePlayState();
    void updateTitle();
    bool handleShortcut(int key, Qt::KeyboardModifiers modifiers);
    void beginSession(bool hosting, const QString& publicAddress, const nlohmann::json& scene, quint64 seq);
    void retireClient();
    void updateSessionUi();
    void updateRemoteMarkers();
    void showControls();
    void onRenderFailed(const QString& message);
    void setUpScripts();
    void onScriptsBuilt(bool ok, const QString& output);
    void newBehaviour();
    void exportGameDialog();
    void saveSelectionAsPrefab();
    entt::entity addPrefabToScene(const std::filesystem::path& path);

    app::AppContext& app_;
    Project project_;
    AssetLibrary assets_;  // project assets (imported models); referenced by the viewport client
    std::unique_ptr<Scene> editScene_;
    std::unique_ptr<Scene> playScene_;
    std::unique_ptr<Simulation> simulation_;
    // Gameplay scripts: Scripts/*.cpp built into the game library, reloaded when they change.
    std::unique_ptr<app::ScriptBuilder> scripts_;
    std::shared_ptr<ScriptModule> gameModule_;
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
    QLabel* statusCollab_ = nullptr;

    QAction* exportGame_ = nullptr;
    QAction* savePrefab_ = nullptr;
    QAction* newBehaviour_ = nullptr;
    QAction* buildScripts_ = nullptr;
    QAction* openScripts_ = nullptr;
    QAction* hostSession_ = nullptr;
    QAction* joinSession_ = nullptr;
    QAction* leaveSession_ = nullptr;
    std::unique_ptr<collab::CollabServer> collabServer_;
    std::unique_ptr<collab::CollabClient> collabClient_;
    std::unique_ptr<collab::CollabSession> collabSession_;
    collab::CollabPanel* collabPanel_ = nullptr;
    QDockWidget* collabDock_ = nullptr;
};

} // namespace lumora::editor
