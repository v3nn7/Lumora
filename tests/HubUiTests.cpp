#include "TestUtils.hpp"

#include "common/AppContext.hpp"
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
    auto combos = page.findChildren<QComboBox*>();
    ASSERT_GE(combos.size(), 2);
    combos[1]->setCurrentIndex(1);  // validation: always on
    EXPECT_EQ(Preferences::load().validation, Preferences::Validation::On);
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
