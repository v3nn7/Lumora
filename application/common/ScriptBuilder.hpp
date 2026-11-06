#pragma once

#include "common/ModuleExport.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <memory>

namespace lumora::app {

/// Compiles a project's gameplay scripts (Scripts/*.cpp) into its game library with CMake and
/// the system's C++ compiler, and optionally rebuilds whenever a script changes.
///
///   <project>/Scripts/*.cpp                      the game's behaviours
///   <project>/Saved/ScriptBuild/src/             generated CMakeLists.txt + entry points
///   <project>/Saved/ScriptBuild/obj/             CMake build tree
///   <project>/Saved/ScriptBuild/bin/game.so|dll  the library (libraryPath())
///   <project>/Saved/ScriptBuild/loaded/          copies in use (ScriptModule::load)
class LUMORA_APPCOMMON_API ScriptBuilder final : public QObject {
    Q_OBJECT
public:
    explicit ScriptBuilder(QString projectRoot, QObject* parent = nullptr);
    ~ScriptBuilder() override;

    [[nodiscard]] QString scriptsDir() const;
    [[nodiscard]] QString buildDir() const;
    [[nodiscard]] QString libraryPath() const;
    [[nodiscard]] QString loadDir() const;
    [[nodiscard]] bool hasScripts() const;
    [[nodiscard]] bool busy() const noexcept { return process_ != nullptr; }
    /// Folder with lumora/Script.hpp (shipped next to the editor, or the source tree).
    [[nodiscard]] static QString sdkIncludeDir();

    /// Starts a build; buildFinished reports the result. A build requested while one runs is
    /// queued and starts right after.
    void build();
    /// Builds synchronously (headless tools and tests). Returns success; `output` gets the log.
    bool buildBlocking(QString* output, int timeoutMs = 600000);
    /// Rebuild automatically (debounced) when files in Scripts/ change.
    void setWatching(bool watching);

    /// Writes Scripts/<className>.cpp from the template. Fails if the file exists.
    bool createBehaviour(const QString& className, QString* error);

signals:
    void buildStarted();
    void buildFinished(bool ok, const QString& output);

private:
    bool prepareSources(QString* error);
    QStringList configureArguments() const;
    void runNext();
    void finish(bool ok);
    void rewatch();

    QString root_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    bool watching_ = false;
    bool queued_ = false;
    QProcess* process_ = nullptr;
    QStringList pendingSteps_;  // "configure" / "build"
    QString output_;
};

} // namespace lumora::app
