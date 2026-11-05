#pragma once

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
class Application final : public QObject {
    Q_OBJECT
public:
    explicit Application(AppContext& context);
    ~Application() override;

    void showHub();
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
};

} // namespace lumora::app
