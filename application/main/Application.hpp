#pragma once

#include "common/ModuleExport.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <vector>

class QWidget;

namespace lumora::editor {
class EditorWindow;
}

namespace lumora::app {

class AppContext;
class HubWindow;
class PlayerWindow;

/// Owns the top-level windows and performs Hub <-> Editor transitions inside one process.
class LUMORA_SHELL_API Application final : public QObject {
    Q_OBJECT
public:
    explicit Application(AppContext& context);
    ~Application() override;

    void showHub();
    /// Hub-only builds (lumora-hub): projects and sessions open in installed editor versions
    /// (separate processes) instead of in this process.
    void setExternalEditors(bool external) noexcept { externalEditors_ = external; }
    /// Starts the installed editor matching the project's engine version (asking before using
    /// another version); false if none could be started.
    bool launchExternalEditor(const QString& projectPath);
    /// Validates and opens a project in the editor; on failure the Hub is shown with the error.
    bool openEditor(const QString& projectPath);
    bool runPlayer(const QString& projectPath);

    /// Asks for an address, connects, and opens the session in the editor (in a local copy of
    /// the host's project, so assets and saves have a home).
    void joinSession();
    /// Joins without asking (e.g. `Lumora --join host:port`); errors are shown in the Hub.
    void joinSessionAt(const QString& address, const QString& name, const QString& password);
    /// Folder of the local project that holds a session's scene and downloaded assets.
    [[nodiscard]] static QString sessionProjectFolder(const QString& projectName);

    [[nodiscard]] HubWindow* hub() const;
    [[nodiscard]] editor::EditorWindow* editor() const;
    void closeEditor();

private:
    AppContext& ctx_;
    QPointer<HubWindow> hub_;
    QPointer<editor::EditorWindow> editor_;
    std::unique_ptr<PlayerWindow> player_;
    std::vector<QPointer<QWidget>> retired_;  // closed windows awaiting deferred deletion
    bool externalEditors_ = false;
};

} // namespace lumora::app
