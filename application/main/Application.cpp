#include "main/Application.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Snapshot.hpp"
#include "core/Log.hpp"
#include "editor/EditorWindow.hpp"
#include "hub/HubWindow.hpp"
#include "player/PlayerWindow.hpp"
#include "scene/SceneSerializer.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QTimer>

namespace lumora::app {

Application::Application(AppContext& context) : ctx_(context) {
    // Developer aid for automated UI checks: LUMORA_UI_SNAPSHOT=/path/prefix writes a PNG of
    // every visible top-level window (with composited Vulkan viewports) after a short delay.
    if (const QString prefix = qEnvironmentVariable("LUMORA_UI_SNAPSHOT"); !prefix.isEmpty()) {
        const int delay = qEnvironmentVariableIntValue("LUMORA_UI_SNAPSHOT_DELAY_MS");
        QTimer::singleShot(delay > 0 ? delay : 3000, this, [prefix] {
            int index = 0;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (!w->isVisible() || w->windowType() != Qt::Window) continue;
                const QString path = QStringLiteral("%1_%2.png").arg(prefix).arg(index++);
                const bool ok = snapshotWidget(w).save(path);
                log::info("App", "UI snapshot {} -> {}", ok ? "written" : "FAILED", path.toStdString());
            }
        });
    }
}

Application::~Application() {
    delete editor_.data();
    delete hub_.data();
    player_.reset();
}

void Application::showHub() {
    if (!hub_) {
        hub_ = new HubWindow(ctx_);
        hub_->setAttribute(Qt::WA_DeleteOnClose);
        connect(hub_, &HubWindow::openProjectRequested, this, [this](const QString& path) {
            // Defer so the Hub's own signal handler finishes before it is closed.
            QTimer::singleShot(0, this, [this, path] { openEditor(path); });
        });
    }
    hub_->refresh();
    hub_->show();
    hub_->raise();
    hub_->activateWindow();
}

bool Application::openEditor(const QString& projectPath) {
    auto project = Project::load(toPath(projectPath));
    if (!project) {
        log::error("App", "cannot open project: {}", project.error().message);
        showHub();
        QMessageBox::critical(hub_, tr("Cannot open project"), QString::fromStdString(project.error().message));
        return false;
    }
    ctx_.recentProjects().touch(project->descriptorPath(), project->name());
    ctx_.saveRecentProjects();

    auto* editor = new editor::EditorWindow(ctx_, std::move(*project));
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &editor::EditorWindow::closeProjectRequested, this, &Application::closeEditor);
    editor_ = editor;
    editor->show();
    if (hub_) hub_->close();  // same process, same Vulkan context: the Hub simply goes away
    return true;
}

void Application::closeEditor() {
    if (editor_) {
        editor_->hide();
        editor_->deleteLater();
        editor_ = nullptr;
    }
    showHub();
}

bool Application::runPlayer(const QString& projectPath) {
    auto project = Project::load(toPath(projectPath));
    if (!project) {
        QMessageBox::critical(nullptr, tr("Lumora Player"), QString::fromStdString(project.error().message));
        return false;
    }
    auto scene = SceneSerializer::load(project->defaultScenePath());
    if (!scene) {
        QMessageBox::critical(nullptr, tr("Lumora Player"), QString::fromStdString(scene.error().message));
        return false;
    }
    player_ = std::make_unique<PlayerWindow>(ctx_, std::move(*project), std::move(*scene));
    connect(player_.get(), &PlayerWindow::closed, qApp, &QApplication::quit, Qt::QueuedConnection);
    player_->show();
    return true;
}

} // namespace lumora::app
