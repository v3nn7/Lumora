#pragma once

#include "common/ModuleExport.hpp"

#include "launcher/EditorInstaller.hpp"
#include "launcher/EditorRegistry.hpp"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QVBoxLayout;

namespace lumora::app {

/// Hub > Editors: editor versions installed side by side, the releases available from the
/// release feed (Cloudflare R2 or any HTTPS / file location), and local builds for development.
class LUMORA_HUB_API EditorsPage final : public QWidget {
    Q_OBJECT
public:
    explicit EditorsPage(QWidget* parent = nullptr);

    /// Fetches the release list (also done the first time the page is shown).
    void refreshFeed();
    /// Note at the top of the page (e.g. why the Hub sent the user here).
    void showMessage(const QString& text, bool error = false);

signals:
    void editorsChanged();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void rebuild();
    void install(const launcher::ReleaseInfo& release);
    QWidget* row(const QString& title, const QString& detail, const QList<QPushButton*>& buttons);

    launcher::EditorInstaller installer_;
    launcher::EditorRegistry registry_;
    std::optional<launcher::ReleaseManifest> manifest_;
    QString feedError_;
    bool fetched_ = false;
    QLineEdit* feed_ = nullptr;
    QComboBox* channel_ = nullptr;
    QPushButton* installLatest_ = nullptr;
    QLabel* message_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QVBoxLayout* installedRows_ = nullptr;
    QVBoxLayout* availableRows_ = nullptr;
};

} // namespace lumora::app
