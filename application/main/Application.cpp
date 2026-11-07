#include "main/Application.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Snapshot.hpp"
#include "core/Log.hpp"
#include "collab/CollabClient.hpp"
#include "collab/CollabUi.hpp"
#include "collab/HostIdentity.hpp"
#include "editor/EditorWindow.hpp"
#include "launcher/EditorRegistry.hpp"
#include "project/Project.hpp"
#include "hub/HubWindow.hpp"
#include "player/PlayerWindow.hpp"
#include "scene/SceneSerializer.hpp"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>

namespace lumora::app {

Application::Application(AppContext& context) : ctx_(context) {
    // Developer aid for automated UI checks: LUMORA_UI_SNAPSHOT=/path/prefix writes a PNG of
    // every visible top-level window (with composited Vulkan viewports) after a short delay.
    // LUMORA_UI_SNAPSHOT_QUIT=1 quits afterwards.
    if (const QString prefix = qEnvironmentVariable("LUMORA_UI_SNAPSHOT"); !prefix.isEmpty()) {
        const int delay = qEnvironmentVariableIntValue("LUMORA_UI_SNAPSHOT_DELAY_MS");
        QTimer::singleShot(delay > 0 ? delay : 3000, this, [this, prefix] {
            int index = 0;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (!w->isVisible() || w->windowType() != Qt::Window) continue;
                const QString path = QStringLiteral("%1_%2.png").arg(prefix).arg(index++);
                const bool ok = snapshotWidget(w).save(path);
                log::info("App", "UI snapshot {} -> {}", ok ? "written" : "FAILED", path.toStdString());
            }
            // LUMORA_UI_SNAPSHOT_QUIT=1: then shut down normally (exercises teardown in smoke runs),
            // without the interactive prompts a close would raise (unsaved scene, ending a session).
            if (qEnvironmentVariableIsSet("LUMORA_UI_SNAPSHOT_QUIT")) {
                if (editor_) {
                    editor_->leaveSession();
                    editor_->context().setDirty(false);
                }
                QCoreApplication::quit();
            }
        });
    }
}

Application::~Application() {
    // Windows closed earlier may still be queued for deleteLater(); they reference the
    // AppContext (renderer, log bridge), so destroy them now, before the context goes away.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const QPointer<QWidget>& w : retired_) delete w.data();
    delete editor_.data();
    delete hub_.data();
    player_.reset();
}

HubWindow* Application::hub() const { return hub_.data(); }

editor::EditorWindow* Application::editor() const { return editor_.data(); }

void Application::showHub() {
    if (!hub_) {
        hub_ = new HubWindow(ctx_);
        hub_->setAttribute(Qt::WA_DeleteOnClose);
        connect(hub_, &HubWindow::openProjectRequested, this, [this](const QString& path) {
            // Defer so the Hub's own signal handler finishes before it is closed.
            QTimer::singleShot(0, this, [this, path] {
                if (externalEditors_) {
                    launchExternalEditor(path);
                } else {
                    openEditor(path);
                }
            });
        });
        connect(hub_, &HubWindow::joinSessionRequested, this, [this] { QTimer::singleShot(0, this, [this] { joinSession(); }); });
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
    connect(editor, &editor::EditorWindow::joinSessionRequested, this,
            [this] { QTimer::singleShot(0, this, [this] { joinSession(); }); });
    editor_ = editor;
    editor->show();
    if (hub_) hub_->close();  // same process, same Vulkan context: the Hub simply goes away
    return true;
}

void Application::closeEditor() {
    if (editor_) {
        editor_->hide();
        editor_->deleteLater();  // we may be inside one of its signal handlers
        retired_.push_back(editor_.data());
        editor_ = nullptr;
    }
    showHub();
}

QString Application::sessionProjectFolder(const QString& projectName) {
    QString safe;
    for (const QChar c : projectName.trimmed()) {
        safe.append(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char(' ')
                        ? c
                        : QLatin1Char('_'));
    }
    safe = safe.trimmed().left(48);
    if (safe.isEmpty()) safe = QStringLiteral("Session");
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Sessions/") + safe);
}

void Application::joinSession() {
    QWidget* parent = editor_ ? static_cast<QWidget*>(editor_.data()) : static_cast<QWidget*>(hub_.data());
    collab::JoinSessionDialog dialog(parent);
    if (dialog.exec() != QDialog::Accepted) return;
    if (externalEditors_) {
        // The Hub has no editor of its own: the newest installed one joins.
        const auto editors = launcher::EditorRegistry().all();
        if (editors.empty()) {
            if (hub_) hub_->showEditorsPage(tr("Install an editor first - it is what joins the session."));
            return;
        }
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LUMORA_SESSION_PASSWORD"), dialog.password());  // not on the command line
        process.setProcessEnvironment(env);
        process.setProgram(editors.front().executable);
        process.setArguments({QStringLiteral("--join"), collab::formatAddress(dialog.address()), QStringLiteral("--user"),
                              dialog.displayName()});
        process.setWorkingDirectory(editors.front().directory);
        if (!process.startDetached()) {
            QMessageBox::warning(hub_, tr("Cannot Join Session"), tr("Lumora %1 could not be started.").arg(editors.front().version));
        }
        return;
    }
    joinSessionAt(collab::formatAddress(dialog.address()), dialog.displayName(), dialog.password());
}

bool Application::launchExternalEditor(const QString& projectPath) {
    auto project = Project::load(toPath(projectPath));
    if (!project) {
        QMessageBox::critical(hub_, tr("Cannot open project"), QString::fromStdString(project.error().message));
        return false;
    }
    const QString wanted = QString::fromStdString(project->engineVersion());
    const launcher::EditorRegistry registry;
    std::optional<launcher::InstalledEditor> editor = registry.exactMatch(wanted);
    if (!editor) {
        const auto all = registry.all();
        if (all.empty()) {
            if (hub_) {
                hub_->showEditorsPage(tr("'%1' needs Lumora %2. Install an editor to open it.")
                                          .arg(QString::fromStdString(project->name()), wanted));
            }
            return false;
        }
        const auto choice = QMessageBox::question(
            hub_, tr("Different Editor Version"),
            tr("'%1' was saved with Lumora %2, which is not installed.\n\nOpen it with Lumora %3? Saving may upgrade "
               "the project.")
                .arg(QString::fromStdString(project->name()), wanted.isEmpty() ? tr("(unknown)") : wanted,
                     all.front().version),
            QMessageBox::Open | QMessageBox::Cancel);
        if (choice != QMessageBox::Open) return false;
        editor = all.front();
    }
    const QString descriptor = toQString(project->descriptorPath());
    if (!QProcess::startDetached(editor->executable, launcher::EditorRegistry::openProjectArguments(descriptor),
                                 editor->directory)) {
        QMessageBox::critical(hub_, tr("Cannot open project"), tr("Lumora %1 could not be started (%2).")
                                                                   .arg(editor->version, editor->executable));
        return false;
    }
    log::info("App", "opened '{}' in Lumora {}", project->name(), editor->version.toStdString());
    ctx_.recentProjects().touch(project->descriptorPath(), project->name());
    ctx_.saveRecentProjects();
    if (hub_) hub_->refresh();
    return true;
}

void Application::joinSessionAt(const QString& addressText, const QString& name, const QString& password) {
    const auto parsed = collab::parseAddress(addressText);
    if (!parsed) {
        showHub();
        QMessageBox::warning(hub_, tr("Cannot Join Session"), tr("'%1' is not an address like host:port.").arg(addressText));
        return;
    }
    const collab::Address address = *parsed;
    if (editor_) {
        editor_->leaveSession();
        closeEditor();  // the editor already offered to save; this shows the Hub
    } else {
        showHub();
    }

    auto client = std::make_unique<collab::CollabClient>();
    collab::CollabClient* raw = client.get();
    // No parent: the Hub closes when the editor opens, while this dialog is still running.
    QProgressDialog progress(tr("Connecting to %1…").arg(collab::formatAddress(address)), tr("Cancel"), 0, 0);
    progress.setWindowTitle(tr("Join Live Session"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    QString failure;
    bool joined = false;
    connect(raw, &collab::CollabClient::joined, &progress, [&](const nlohmann::json& scene, quint64 seq) {
        // Runs synchronously inside the client's message handling, so no later message can
        // arrive before the editor has adopted the session.
        const QString folder = sessionProjectFolder(raw->projectName());
        auto project = Project::load(toPath(folder));
        if (!project) project = Project::create(toPath(folder), QDir(folder).dirName().toStdString(), ProjectTemplate::Empty);
        if (!project) {
            failure = tr("Cannot create a local copy of the project in %1: %2")
                          .arg(folder, QString::fromStdString(project.error().message));
            progress.cancel();
            return;
        }
        raw->setProjectRoot(toQString(project->rootDir()));
        if (!openEditor(toQString(project->descriptorPath()))) {
            failure = tr("Cannot open the session's project.");
            progress.cancel();
            return;
        }
        joined = true;
        editor_->attachSession(std::move(client), scene, seq);
        progress.accept();
    });
    connect(raw, &collab::CollabClient::joinFailed, &progress, [&](const QString& reason) {
        failure = reason;
        progress.cancel();
    });
    QByteArray unconfirmed;  // fingerprint of a host seen for the first time
    connect(raw, &collab::CollabClient::untrustedHost, &progress,
            [&](const QString&, quint16, const QByteArray& fingerprint) { unconfirmed = fingerprint; });
    raw->connectTo(address, name, password);
    progress.exec();
    if (joined) return;
    raw->disconnect(&progress);  // cancelled or failed: no more callbacks into this frame
    raw->disconnectFromHost();
    if (!unconfirmed.isEmpty()) {
        showHub();
        const auto answer = QMessageBox::question(
            hub_, tr("Confirm the Host"),
            tr("<p>This is your first connection to <b>%1</b>. The session is encrypted, but its "
               "certificate is the host's own, so confirm it belongs to them:</p>"
               "<p style='font-family: monospace; font-size: 11pt'>%2</p>"
               "<p>Ask the host to read the fingerprint shown in their <i>Session</i> panel. Trust "
               "and join only if it matches. (Addresses copied from that panel include it, and skip "
               "this question.)</p>")
                .arg(collab::formatAddress({address.host, address.port, {}}).toHtmlEscaped(),
                     collab::formatFingerprint(unconfirmed)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) {
            collab::KnownHosts::remember(address.host, address.port, unconfirmed);
            QTimer::singleShot(0, this, [this, addressText, name, password] { joinSessionAt(addressText, name, password); });
        }
        return;
    }
    if (!failure.isEmpty()) {
        showHub();
        QMessageBox::warning(hub_, tr("Cannot Join Session"), failure);
    }
}

bool Application::runPlayer(const QString& projectPath, const GameNetworkOptions& network) {
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
    player_ = std::make_unique<PlayerWindow>(ctx_, std::move(*project), std::move(*scene), network);
    connect(player_.get(), &PlayerWindow::closed, qApp, &QApplication::quit, Qt::QueuedConnection);
    player_->show();
    return true;
}

} // namespace lumora::app
