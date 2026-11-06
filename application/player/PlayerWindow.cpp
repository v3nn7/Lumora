#include "player/PlayerWindow.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/ScriptBuilder.hpp"
#include "common/Theme.hpp"
#include "core/Log.hpp"
#include "runtime/AudioWorld.hpp"
#include "runtime/Scripting.hpp"
#include "runtime/Simulation.hpp"
#include "scene/Scene.hpp"

#include <QEvent>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

namespace lumora::app {

PlayerWindow::PlayerWindow(AppContext& app, Project project, std::unique_ptr<Scene> scene)
    : app_(app), project_(std::move(project)), assets_(project_.rootDir()), scene_(std::move(scene)) {
    scene_->updateWorldTransforms();
    simulation_ = std::make_unique<Simulation>(*scene_);
    simulation_->setProjectRoot(project_.rootDir());
    // Saves go to the user's data folder: an installed game's own folder may be read-only.
    const QString saves = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                          QStringLiteral("/LumoraGames/") + QString::fromStdString(project_.name());
    simulation_->setSaveFile(toPath(saves) / "save.json");
    const CameraView initial = sceneCameraView(*scene_, 16.0f / 9.0f);
    freeCamera_.setPosition(initial.position);
    freeCam_ = scene_->primaryCamera() == entt::null;

    window_ = std::make_unique<VulkanViewport>(app_, *this);
    window_->setTitle(QStringLiteral("%1 — Lumora Player").arg(QString::fromStdString(project_.name())));
    window_->setIcon(theme::icon(QStringLiteral("logo")));
    window_->resize(1280, 720);
    window_->installEventFilter(this);
    if (AudioWorld::sceneHasAudio(*scene_)) {
        auto audio = AudioWorld::create(*scene_, {project_.rootDir(), true});
        if (audio) {
            simulation_->setAudio(std::move(*audio));
        } else {
            log::warn("Player", "{} - playing without sound", audio.error().message);
        }
    }
    // The game's behaviours, as last built by the editor (the player does not compile).
    const ScriptBuilder scripts(toQString(project_.rootDir()));
    if (QFileInfo::exists(scripts.libraryPath())) {
        auto module = ScriptModule::load(toPath(scripts.libraryPath()), toPath(scripts.loadDir()));
        if (module) {
            simulation_->setScripts(*module, &window_->input());
        } else {
            log::error("Player", "{}", module.error().message);
        }
    }
    connect(window_.get(), &VulkanViewport::renderFailed, this, [this](const QString& message) {
        QMessageBox::critical(nullptr, tr("Lumora Player"), message);
        window_->close();
    });
}

PlayerWindow::~PlayerWindow() { window_.reset(); }

void PlayerWindow::show() {
    window_->show();
    log::info("Player", "running '{}' ({} entities)", project_.name(), scene_->entityCount());
}

bool PlayerWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_.get() && event->type() == QEvent::Close) {
        emit closed();
    }
    return QObject::eventFilter(watched, event);
}

void PlayerWindow::viewportTick(float dt, const InputState& input, bool) {
    const qreal dpr = window_->devicePixelRatio();
    simulation_->hud().screenSize = glm::vec2(window_->width() * dpr, window_->height() * dpr);
    simulation_->hud().pixelRatio = static_cast<float>(dpr);
    const std::uint64_t loads = simulation_->sceneLoads();
    simulation_->advance(dt);
    if (simulation_->sceneLoads() != loads) freeCam_ = scene_->primaryCamera() == entt::null;  // the new level's camera
    if (freeCam_) freeCamera_.update(input, dt);
    if (simulation_->audio() != nullptr) simulation_->audio()->setListenerFromView(viewportCamera(1.0f).view);
}

const Scene* PlayerWindow::viewportScene() const { return scene_.get(); }

CameraView PlayerWindow::viewportCamera(float aspect) const {
    return freeCam_ ? freeCamera_.view(aspect) : sceneCameraView(*scene_, aspect);
}

render::RenderSettings PlayerWindow::viewportSettings() const {
    render::RenderSettings s;
    s.assets = &assets_;
    s.hud = &simulation_->hud();
    s.debug = showDebug_ ? &simulation_->debugDraw() : nullptr;
    return s;
}

bool PlayerWindow::viewportKeyPressed(int key, Qt::KeyboardModifiers) {
    switch (key) {
        case Qt::Key_Escape:
            // A game with behaviours gets Escape (pause menus); one without quits with it.
            if (simulation_->scriptInstances() > 0) return false;
            window_->close();
            return true;
        case Qt::Key_F11:
            window_->setWindowStates(window_->windowStates() ^ Qt::WindowFullScreen);
            return true;
        case Qt::Key_F3:
            showDebug_ = !showDebug_;  // the behaviours' debug shapes
            return true;
        case Qt::Key_F8:
            freeCam_ = !freeCam_ || scene_->primaryCamera() == entt::null;
            return true;
        default:
            return false;
    }
}

} // namespace lumora::app
