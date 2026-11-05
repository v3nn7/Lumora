#pragma once

#include <QObject>
#include <QPointer>

#include <memory>

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

private:
    void closeEditor();

    AppContext& ctx_;
    QPointer<HubWindow> hub_;
    QPointer<editor::EditorWindow> editor_;
    std::unique_ptr<PlayerWindow> player_;
};

} // namespace lumora::app
