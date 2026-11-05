#include "hub/EditorsPage.hpp"

#include "launcher/Signature.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace lumora::app {

namespace {

QFrame* card(const QString& title, const QString& subtitle, QVBoxLayout*& content) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("editorsCard"));
    frame->setStyleSheet(QStringLiteral("#editorsCard { background: #16181d; border: 1px solid #262a32; border-radius: 12px; }"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(20, 16, 20, 18);
    layout->setSpacing(4);
    auto* heading = new QLabel(title);
    heading->setStyleSheet(QStringLiteral("font-size: 12pt; font-weight: 650; color: #eef0f3;"));
    auto* sub = new QLabel(subtitle);
    sub->setProperty("muted", true);
    sub->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(sub);
    layout->addSpacing(8);
    content = new QVBoxLayout;
    content->setSpacing(6);
    layout->addLayout(content);
    return frame;
}

void clear(QVBoxLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

QString megabytes(quint64 bytes) { return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / 1e6, 0, 'f', 1); }

} // namespace

EditorsPage::EditorsPage(QWidget* parent)
    : QWidget(parent), installer_(launcher::EditorInstaller::defaultInstallRoot(), this) {
    installer_.setPublicKey(launcher::releasePublicKey());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(30, 26, 30, 20);
    layout->setSpacing(16);
    auto* title = new QLabel(tr("Editors"));
    title->setStyleSheet(QStringLiteral("font-size: 18pt; font-weight: 700; color: #f0f2f5;"));
    layout->addWidget(title);

    message_ = new QLabel;
    message_->setWordWrap(true);
    message_->hide();
    layout->addWidget(message_);
    auto* progressRow = new QHBoxLayout;
    progress_ = new QProgressBar;
    progress_->hide();
    cancel_ = new QPushButton(tr("Cancel"));
    cancel_->hide();
    connect(cancel_, &QPushButton::clicked, &installer_, &launcher::EditorInstaller::cancel);
    progressRow->addWidget(progress_, 1);
    progressRow->addWidget(cancel_);
    layout->addLayout(progressRow);

    layout->addWidget(card(tr("Installed"),
                           tr("Each project opens in the editor version it was made with. Versions are installed "
                              "side by side in %1.")
                               .arg(installer_.installRoot().toHtmlEscaped()),
                           installedRows_));

    QVBoxLayout* feedContent = nullptr;
    layout->addWidget(card(tr("Available"),
                           tr("Releases published to the release feed. Downloads are checked against the SHA-256 in "
                              "the feed%1.")
                               .arg(launcher::releasePublicKey().isEmpty()
                                        ? tr(" (this build accepts unsigned feeds)")
                                        : tr(", whose Ed25519 signature must match Lumora's release key")),
                           feedContent));
    auto* feedRow = new QHBoxLayout;
    feed_ = new QLineEdit(launcher::EditorRegistry::feedUrl());
    feed_->setPlaceholderText(tr("https://<your bucket>.r2.dev/editor/manifest.json"));
    auto* refresh = new QPushButton(tr("Refresh"));
    connect(refresh, &QPushButton::clicked, this, [this] {
        launcher::EditorRegistry::setFeedUrl(feed_->text());
        refreshFeed();
    });
    connect(feed_, &QLineEdit::returnPressed, refresh, &QPushButton::click);
    feedRow->addWidget(feed_, 1);
    feedRow->addWidget(refresh);
    feedContent->addLayout(feedRow);

    // Channels: stable releases, plus betas, plus alphas (each includes the more stable ones).
    auto* channelRow = new QHBoxLayout;
    auto* channelLabel = new QLabel(tr("Show"));
    channel_ = new QComboBox;
    channel_->addItem(tr("Stable releases"), static_cast<int>(launcher::Channel::Stable));
    channel_->addItem(tr("Stable and beta"), static_cast<int>(launcher::Channel::Beta));
    channel_->addItem(tr("Everything, including alpha"), static_cast<int>(launcher::Channel::Alpha));
    channel_->setCurrentIndex(channel_->findData(static_cast<int>(launcher::EditorRegistry::preferredChannel())));
    connect(channel_, &QComboBox::currentIndexChanged, this, [this] {
        launcher::EditorRegistry::setPreferredChannel(static_cast<launcher::Channel>(channel_->currentData().toInt()));
        rebuild();
    });
    installLatest_ = new QPushButton(tr("Install Latest"));
    installLatest_->setProperty("accent", true);
    connect(installLatest_, &QPushButton::clicked, this, [this] {
        if (!manifest_) return;
        const auto upTo = static_cast<launcher::Channel>(channel_->currentData().toInt());
        if (const launcher::ReleaseInfo* latest = manifest_->latest(launcher::currentPlatform(), upTo)) install(*latest);
    });
    channelRow->addWidget(channelLabel);
    channelRow->addWidget(channel_);
    channelRow->addStretch(1);
    channelRow->addWidget(installLatest_);
    feedContent->addLayout(channelRow);
    availableRows_ = new QVBoxLayout;
    availableRows_->setSpacing(6);
    feedContent->addLayout(availableRows_);
    layout->addStretch(1);

    connect(&installer_, &launcher::EditorInstaller::manifestReady, this, [this](const launcher::ReleaseManifest& m) {
        manifest_ = m;
        feedError_.clear();
        rebuild();
    });
    connect(&installer_, &launcher::EditorInstaller::manifestFailed, this, [this](const QString& error) {
        manifest_.reset();
        feedError_ = error;
        rebuild();
    });
    connect(&installer_, &launcher::EditorInstaller::progress, this, [this](qint64 received, qint64 total) {
        progress_->setRange(0, 1000);
        progress_->setValue(total > 0 ? static_cast<int>(received * 1000 / total) : 0);
        progress_->setFormat(QStringLiteral("%1 / %2").arg(megabytes(static_cast<quint64>(received)),
                                                           megabytes(static_cast<quint64>(total))));
    });
    connect(&installer_, &launcher::EditorInstaller::installCompleted, this, [this](const launcher::InstalledEditor& e) {
        progress_->hide();
        cancel_->hide();
        showMessage(tr("Lumora %1 is installed.").arg(e.version));
        rebuild();
        emit editorsChanged();
    });
    connect(&installer_, &launcher::EditorInstaller::installFailed, this, [this](const QString& error) {
        progress_->hide();
        cancel_->hide();
        showMessage(error, true);
        rebuild();
    });
    rebuild();
}

void EditorsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    rebuild();
    if (!fetched_ && !feed_->text().trimmed().isEmpty()) refreshFeed();
}

void EditorsPage::refreshFeed() {
    fetched_ = true;
    const QString url = feed_->text().trimmed();
    if (url.isEmpty()) {
        feedError_ = tr("Enter the address of the release feed (manifest.json).");
        rebuild();
        return;
    }
    feedError_ = tr("Loading…");
    rebuild();
    installer_.fetchManifest(QUrl::fromUserInput(url));
}

void EditorsPage::showMessage(const QString& text, bool error) {
    message_->setText(text);
    message_->setStyleSheet(error ? QStringLiteral("color: #ec6a5e;") : QStringLiteral("color: #7be0a0;"));
    message_->setVisible(!text.isEmpty());
}

QWidget* EditorsPage::row(const QString& title, const QString& detail, const QList<QPushButton*>& buttons) {
    auto* w = new QWidget;
    auto* layout = new QHBoxLayout(w);
    layout->setContentsMargins(0, 2, 0, 2);
    auto* text = new QLabel(QStringLiteral("<b>%1</b><br><span style='color:#7c8494'>%2</span>")
                                .arg(title.toHtmlEscaped(), detail.toHtmlEscaped()));
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(text, 1);
    for (QPushButton* b : buttons) layout->addWidget(b);
    return w;
}

void EditorsPage::rebuild() {
    clear(installedRows_);
    const auto editors = registry_.all();
    for (const launcher::InstalledEditor& e : editors) {
        const bool local = e.source == QLatin1String("local");
        auto* folder = new QPushButton(tr("Show Folder"));
        connect(folder, &QPushButton::clicked, this,
                [dir = e.directory] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
        auto* remove = new QPushButton(local ? tr("Forget") : tr("Remove"));
        connect(remove, &QPushButton::clicked, this, [this, e, local] {
            if (local) {
                registry_.removeLocalBuild(e.executable);
            } else {
                if (QMessageBox::question(this, tr("Remove Editor"), tr("Remove Lumora %1 from this computer?").arg(e.version)) !=
                    QMessageBox::Yes) {
                    return;
                }
                if (auto removed = installer_.uninstall(e.version); !removed) {
                    showMessage(QString::fromStdString(removed.error().message), true);
                }
            }
            rebuild();
            emit editorsChanged();
        });
        remove->setEnabled(!installer_.busy());
        QString title = tr("Lumora %1").arg(e.version);
        if (launcher::channelOfVersion(e.version) != launcher::Channel::Stable) {
            title += QStringLiteral("  · ") + launcher::channelName(launcher::channelOfVersion(e.version));
        }
        if (local) title += tr("  (local build)");
        installedRows_->addWidget(row(title, e.executable, {folder, remove}));
    }
    if (editors.empty()) {
        auto* none = new QLabel(tr("No editor installed yet - install one below, or add a local build."));
        none->setProperty("muted", true);
        installedRows_->addWidget(none);
    }
    auto* addLocal = new QPushButton(tr("Add Local Build…"));
    addLocal->setToolTip(tr("Use an editor you built yourself (build/release/bin/lumora-editor)"));
    connect(addLocal, &QPushButton::clicked, this, [this] {
        const QString exe = QFileDialog::getOpenFileName(this, tr("Choose lumora-editor"));
        if (exe.isEmpty()) return;
        if (auto added = registry_.addLocalBuild(exe); added) {
            showMessage(tr("Added local build %1.").arg(added->version));
        } else {
            showMessage(QString::fromStdString(added.error().message), true);
        }
        rebuild();
        emit editorsChanged();
    });
    auto* addRow = new QHBoxLayout;
    addRow->addStretch(1);
    addRow->addWidget(addLocal);
    auto* addHolder = new QWidget;
    addHolder->setLayout(addRow);
    installedRows_->addWidget(addHolder);

    clear(availableRows_);
    installLatest_->setEnabled(false);
    if (!manifest_) {
        if (!feedError_.isEmpty()) {
            auto* note = new QLabel(feedError_);
            note->setWordWrap(true);
            note->setProperty("muted", true);
            availableRows_->addWidget(note);
        }
        return;
    }
    const QString platform = launcher::currentPlatform();
    const auto upTo = static_cast<launcher::Channel>(channel_->currentData().toInt());
    if (const launcher::ReleaseInfo* latest = manifest_->latest(platform, upTo)) {
        const bool have = installer_.find(latest->version).has_value();
        installLatest_->setEnabled(!have && !installer_.busy());
        installLatest_->setText(have ? tr("Latest Installed") : tr("Install %1").arg(latest->version));
    }
    int shown = 0;
    for (const launcher::ReleaseInfo* release : manifest_->visible(platform, upTo)) {
        const launcher::ReleaseInfo& r = *release;
        const launcher::PackageInfo* package = r.packageFor(platform);
        const bool have = installer_.find(r.version).has_value();
        auto* installButton = new QPushButton(have ? tr("Installed") : tr("Install"));
        installButton->setEnabled(!have && !installer_.busy());
        if (!have) connect(installButton, &QPushButton::clicked, this, [this, r] { install(r); });
        QString detail = QStringLiteral("%1 · %2 · %3").arg(r.channel, r.date, megabytes(package->size));
        if (!r.notes.isEmpty()) detail += QStringLiteral(" · ") + r.notes;
        availableRows_->addWidget(row(tr("Lumora %1").arg(r.version), detail, {installButton}));
        ++shown;
    }
    if (shown == 0) {
        auto* none = new QLabel(tr("No %1 releases for %2 in the feed.")
                                    .arg(upTo == launcher::Channel::Stable ? tr("stable") : tr("matching"), platform));
        none->setProperty("muted", true);
        availableRows_->addWidget(none);
    }
}

void EditorsPage::install(const launcher::ReleaseInfo& release) {
    const launcher::PackageInfo* package = release.packageFor(launcher::currentPlatform());
    if (package == nullptr) return;
    showMessage(tr("Installing Lumora %1…").arg(release.version));
    progress_->setRange(0, 0);
    progress_->show();
    cancel_->show();
    installer_.install(release, *package);
    rebuild();  // disable the other buttons while it runs
}

} // namespace lumora::app
