#include "TestUtils.hpp"

#include "common/AppContext.hpp"
#include "core/Version.hpp"
#include "common/Preferences.hpp"
#include "common/ProjectUi.hpp"
#include "hub/HubSettingsPage.hpp"
#include "hub/HubWindow.hpp"
#include "project/Project.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
#include <QSettings>

using namespace lumora;
using namespace lumora::app;

TEST(Preferences, PersistAndCommandLineWins) {
    QSettings().clear();
    Preferences p;
    p.gpuIndex = 1;
    p.validation = Preferences::Validation::Off;
    p.vsync = false;
    p.projectsDirectory = QStringLiteral("/tmp/projects");
    p.save();

    const Preferences loaded = Preferences::load();
    EXPECT_EQ(loaded.gpuIndex, std::optional<std::uint32_t>(1));
    EXPECT_EQ(loaded.validation, Preferences::Validation::Off);
    EXPECT_FALSE(loaded.vsync);
    EXPECT_EQ(loaded.effectiveProjectsDirectory(), QStringLiteral("/tmp/projects"));

    AppOptions fromPrefs;
    loaded.applyTo(fromPrefs);
    EXPECT_EQ(fromPrefs.gpuIndex, std::optional<std::uint32_t>(1));
    EXPECT_EQ(fromPrefs.validation, std::optional<bool>(false));
    EXPECT_EQ(fromPrefs.vsync, std::optional<bool>(false));

    AppOptions cli;
    cli.gpuIndex = 0;
    cli.validation = true;
    cli.vsync = true;
    loaded.applyTo(cli);
    EXPECT_EQ(cli.gpuIndex, std::optional<std::uint32_t>(0)) << "command line takes precedence";
    EXPECT_EQ(cli.validation, std::optional<bool>(true));
    EXPECT_EQ(cli.vsync, std::optional<bool>(true));
    QSettings().clear();
}

TEST(HubSettings, ChangesAreSavedImmediately) {
    QSettings().clear();
    AppContext context(AppOptions{});
    HubSettingsPage page(context);
    page.resize(900, 700);
    auto* vsync = page.findChild<QCheckBox*>();
    ASSERT_NE(vsync, nullptr);
    EXPECT_TRUE(vsync->isChecked()) << "default preference";
    vsync->setChecked(false);
    EXPECT_FALSE(Preferences::load().vsync);
    auto* validation = page.findChild<QComboBox*>(QStringLiteral("validation"));
    ASSERT_NE(validation, nullptr);
    validation->setCurrentIndex(1);  // always on
    EXPECT_EQ(Preferences::load().validation, Preferences::Validation::On);
    auto* backend = page.findChild<QComboBox*>(QStringLiteral("backend"));
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(Preferences::load().backend, QStringLiteral("vulkan")) << "Vulkan by default";
    if (const QString dir = qEnvironmentVariable("LUMORA_TEST_SNAPSHOT_DIR"); !dir.isEmpty()) {
        page.show();
        QApplication::processEvents();
        page.grab().save(dir + QStringLiteral("/hub_settings.png"));
    }
    QSettings().clear();
}

TEST(HubWindowTest, DropAddsExistingProjectAndSelectsIt) {
    QSettings().clear();
    test::TempDir dir;
    auto project = Project::create(dir.path() / "Dropped", "Dropped", ProjectTemplate::Empty);
    ASSERT_TRUE(project.has_value());
    AppContext context(AppOptions{});
    // Start from an empty recent list regardless of earlier tests.
    while (!context.recentProjects().entries().empty()) {
        context.recentProjects().remove(context.recentProjects().entries().front().descriptor);
    }
    HubWindow hub(context);
    auto* list = hub.findChild<QListView*>(QStringLiteral("projectList"));
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->model()->rowCount(), 0);
    // Dropping the project folder resolves its .lumora descriptor.
    ASSERT_TRUE(hub.addExistingProject(toQString(dir.path() / "Dropped")));
    EXPECT_EQ(list->model()->rowCount(), 1);
    ASSERT_TRUE(list->currentIndex().isValid()) << "the added project is selected";
    EXPECT_EQ(list->currentIndex().data(Qt::DisplayRole).toString(), QStringLiteral("Dropped"));
    EXPECT_EQ(context.recentProjects().entries().size(), 1u);
    QSettings().clear();
}

#if !defined(Q_OS_WIN)
#include "common/Snapshot.hpp"
#include "launcher/EditorRegistry.hpp"
#include "main/Application.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QThread>

TEST(HubEditors, ProjectsOpenInTheMatchingInstalledEditor) {
    // lumora-hub mode: opening a project starts the editor version it was made with, as a
    // separate process. A tiny script stands in for an installed lumora-editor.
    QSettings().clear();
    test::TempDir dir;
    const QString log = QString::fromStdString((dir.path() / "launched.txt").string());
    const QString editor = QString::fromStdString((dir.path() / "lumora-editor").string());
    {
        QFile script(editor);
        ASSERT_TRUE(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral("#!/bin/sh\nif [ \"$1\" = --version ]; then echo 'Lumora Engine %1 (Release)'; exit 0; fi\n"
                                    "printf '%s\\n' \"$@\" > '%2'\n")
                         .arg(QString::fromLatin1(version::kString), log)
                         .toUtf8());
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    launcher::EditorRegistry registry(QString::fromStdString((dir.path() / "installs").string()));
    auto added = registry.addLocalBuild(editor);
    ASSERT_TRUE(added.has_value()) << added.error().message;
    EXPECT_EQ(added->version, QString::fromLatin1(version::kString));
    ASSERT_EQ(registry.localBuilds().size(), 1u);

    auto project = Project::create(dir.path() / "Game", "Game", ProjectTemplate::Empty);
    ASSERT_TRUE(project.has_value()) << project.error().message;
    AppContext context(AppOptions{});
    Application application(context);
    application.setExternalEditors(true);
    ASSERT_TRUE(application.launchExternalEditor(toQString(project->descriptorPath())));

    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(log) && timer.elapsed() < 5000) QThread::msleep(20);
    QFile launched(log);
    ASSERT_TRUE(launched.open(QIODevice::ReadOnly)) << "the editor process was not started";
    const QStringList args = QString::fromUtf8(launched.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    EXPECT_EQ(args, (QStringList{QStringLiteral("--editor"), toQString(project->descriptorPath())}));
    EXPECT_EQ(application.editor(), nullptr) << "the Hub does not open an editor in-process";

    // The Hub's Editors page lists the build (snapshot for visual checks when requested).
    HubWindow hub(context);
    hub.resize(1240, 780);
    hub.showEditorsPage(QStringLiteral("'Game' needs Lumora 9.9.9. Install an editor to open it."));
    QApplication::processEvents();
    if (const QString out = qEnvironmentVariable("LUMORA_TEST_SNAPSHOT_DIR"); !out.isEmpty()) {
        snapshotWidget(&hub).save(out + QStringLiteral("/hub_editors.png"));
    }

    registry.removeLocalBuild(added->executable);
    EXPECT_TRUE(registry.localBuilds().empty());
    QSettings().clear();
}
#endif
