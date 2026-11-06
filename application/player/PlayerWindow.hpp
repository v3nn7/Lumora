#pragma once

#include "common/ModuleExport.hpp"

#include "common/VulkanViewport.hpp"
#include "assets/AssetLibrary.hpp"
#include "project/Project.hpp"

#include <QObject>

#include <memory>

namespace lumora {
class Scene;
class Simulation;
}

namespace lumora::app {

class AppContext;

/// Standalone runtime (Lumora --project X --play): loads the default scene and runs the
/// simulation, viewed through the scene's primary camera. Esc quits, F11 toggles fullscreen,
/// F8 toggles a free-fly debug camera.
class LUMORA_PLAYER_API PlayerWindow final : public QObject, public ViewportClient {
    Q_OBJECT
public:
    PlayerWindow(AppContext& app, Project project, std::unique_ptr<Scene> scene);
    ~PlayerWindow() override;

    void show();

    void viewportTick(float dt, const InputState& input, bool hasFocus) override;
    [[nodiscard]] const Scene* viewportScene() const override;
    [[nodiscard]] CameraView viewportCamera(float aspect) const override;
    [[nodiscard]] render::RenderSettings viewportSettings() const override;
    bool viewportKeyPressed(int key, Qt::KeyboardModifiers modifiers) override;

signals:
    void closed();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    AppContext& app_;
    Project project_;
    mutable AssetLibrary assets_;  // caches lazily while rendering
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<Simulation> simulation_;
    std::unique_ptr<VulkanViewport> window_;
    FlyCameraController freeCamera_;
    bool freeCam_ = false;
    bool showDebug_ = false;
};

} // namespace lumora::app
