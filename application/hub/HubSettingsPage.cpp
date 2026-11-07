#include "hub/HubSettingsPage.hpp"

#include "common/AppContext.hpp"
#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/d3d12/D3D12Context.hpp"
#include "renderer/vulkan/Device.hpp"
#include "renderer/vulkan/Instance.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <format>

namespace lumora::app {

namespace {

QFrame* card(const QString& title, const QString& subtitle, QFormLayout*& form) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("settingsCard"));
    frame->setStyleSheet(QStringLiteral("#settingsCard { background: #16181d; border: 1px solid #262a32; border-radius: 12px; }"));
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
    layout->addSpacing(10);
    form = new QFormLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);
    return frame;
}

} // namespace

HubSettingsPage::HubSettingsPage(AppContext& app, QWidget* parent)
    : QWidget(parent), app_(app), prefs_(Preferences::load()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 26, 30, 20);
    layout->setSpacing(16);
    auto* title = new QLabel(tr("Settings"));
    title->setStyleSheet(QStringLiteral("font-size: 18pt; font-weight: 700; color: #f0f2f5;"));
    layout->addWidget(title);

    // Graphics
    QFormLayout* graphics = nullptr;
    layout->addWidget(card(tr("Graphics"), tr("Graphics API, device and debugging options. Command-line flags "
                                              "(--backend, --gpu, --validation, --no-vsync) take precedence."),
                           graphics));
    backend_ = new QComboBox;
    backend_->setObjectName(QStringLiteral("backend"));
    backend_->addItem(tr("Vulkan"), QStringLiteral("vulkan"));
    backend_->addItem(tr("Direct3D 12"), QStringLiteral("d3d12"));
    if (!render::d3d12::compiledIn()) {
        // Only Windows builds have it.
        auto* model = qobject_cast<QStandardItemModel*>(backend_->model());
        if (model != nullptr) model->item(1)->setEnabled(false);
        backend_->setItemText(1, tr("Direct3D 12 (Windows only)"));
    }
    gpu_ = new QComboBox;
    gpu_->setObjectName(QStringLiteral("gpu"));
    gpu_->setMinimumWidth(360);
    activeGpu_ = new QLabel;
    activeGpu_->setProperty("muted", true);
    validation_ = new QComboBox;
    validation_->setObjectName(QStringLiteral("validation"));
    validation_->addItems({tr("Automatic (on in Debug builds)"), tr("Always on"), tr("Off")});
    vsync_ = new QCheckBox(tr("Pace frames to the display refresh rate"));
    graphics->addRow(tr("Graphics API"), backend_);
    graphics->addRow(tr("GPU"), gpu_);
    graphics->addRow(QString(), activeGpu_);
    graphics->addRow(tr("Validation layers"), validation_);
    graphics->addRow(tr("V-Sync"), vsync_);

    // Projects
    QFormLayout* projects = nullptr;
    layout->addWidget(card(tr("Projects"), tr("Where new projects are created and recent-list maintenance."), projects));
    auto* locationRow = new QHBoxLayout;
    location_ = new QLineEdit;
    location_->setPlaceholderText(Preferences{}.effectiveProjectsDirectory());
    auto* browse = new QPushButton(theme::icon(QStringLiteral("open")), tr("Browse…"));
    locationRow->addWidget(location_, 1);
    locationRow->addWidget(browse);
    projects->addRow(tr("Default location"), locationRow);
    auto* prune = new QPushButton(theme::icon(QStringLiteral("trash")), tr("Remove missing projects from the list"));
    projects->addRow(QString(), prune);

    // Editor
    QFormLayout* editor = nullptr;
    layout->addWidget(card(tr("Editor"), tr("Window layout is remembered between sessions."), editor));
    auto* resetLayout = new QPushButton(tr("Reset editor window layout"));
    editor->addRow(QString(), resetLayout);

    status_ = new QLabel;
    status_->setProperty("muted", true);
    layout->addWidget(status_);
    layout->addStretch();

    loading_ = true;
    validation_->setCurrentIndex(static_cast<int>(prefs_.validation));
    backend_->setCurrentIndex(std::max(backend_->findData(prefs_.backend), 0));
    vsync_->setChecked(prefs_.vsync);
    location_->setText(prefs_.projectsDirectory);
    loading_ = false;

    connect(backend_, &QComboBox::currentIndexChanged, this, &HubSettingsPage::commit);
    connect(gpu_, &QComboBox::currentIndexChanged, this, &HubSettingsPage::commit);
    connect(validation_, &QComboBox::currentIndexChanged, this, &HubSettingsPage::commit);
    connect(vsync_, &QCheckBox::toggled, this, &HubSettingsPage::commit);
    connect(location_, &QLineEdit::editingFinished, this, &HubSettingsPage::commit);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Default project location"),
                                                              prefs_.effectiveProjectsDirectory());
        if (!dir.isEmpty()) {
            location_->setText(dir);
            commit();
        }
    });
    connect(prune, &QPushButton::clicked, this, [this] {
        int removed = 0;
        const auto entries = app_.recentProjects().entries();  // copy: remove() mutates
        for (const RecentProject& p : entries) {
            if (!QFileInfo::exists(toQString(p.descriptor))) removed += app_.recentProjects().remove(p.descriptor) ? 1 : 0;
        }
        app_.saveRecentProjects();
        emit recentProjectsChanged();
        status_->setText(removed == 1 ? tr("Removed 1 missing project.") : tr("Removed %1 missing projects.").arg(removed));
    });
    connect(resetLayout, &QPushButton::clicked, this, [this] {
        QSettings settings;
        settings.remove(QStringLiteral("editor/state"));
        settings.remove(QStringLiteral("editor/geometry"));
        status_->setText(tr("The editor will open with the default layout."));
    });
}

void HubSettingsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!populated_) {
        populated_ = true;
        QTimer::singleShot(0, this, &HubSettingsPage::populateGpus);
    }
}

void HubSettingsPage::populateGpus() {
    loading_ = true;
    gpu_->clear();
    gpu_->addItem(tr("Automatic — best suitable GPU"), -1);
    try {
        rhi::InstanceConfig config;
        config.applicationName = "Lumora (settings)";
        rhi::Instance instance(config);
        auto* model = qobject_cast<QStandardItemModel*>(gpu_->model());
        for (const auto& c : rhi::enumerateDevices(instance.handle(), true)) {
            const QString label = QString::fromStdString(std::format("[{}] {} — {} — Vulkan {}", c.index, c.caps.name,
                                                                     c.caps.typeName(), rhi::versionString(c.caps.apiVersion)));
            gpu_->addItem(c.suitable ? label : label + tr("  (unsuitable)"), static_cast<int>(c.index));
            const int row = gpu_->count() - 1;
            if (!c.suitable && model != nullptr) {
                model->item(row)->setEnabled(false);
                model->item(row)->setToolTip(QString::fromStdString(c.rejectReason));
            }
        }
    } catch (const std::exception& e) {
        gpu_->addItem(tr("Vulkan unavailable: %1").arg(QString::fromUtf8(e.what())), -2);
        gpu_->setEnabled(false);
    }
    const int wanted = prefs_.gpuIndex ? static_cast<int>(*prefs_.gpuIndex) : -1;
    const int found = gpu_->findData(wanted);
    gpu_->setCurrentIndex(found >= 0 ? found : 0);
    if (render::RenderContext* rc = app_.renderContext()) {
        activeGpu_->setText(tr("In use now: %1").arg(QString::fromStdString(rc->caps().name)));
    } else {
        activeGpu_->setText(app_.rendererError());
    }
    loading_ = false;
}

void HubSettingsPage::commit() {
    if (loading_) return;
    const Preferences before = prefs_;
    const int gpu = gpu_->currentData().toInt();
    if (gpu_->isEnabled() && populated_) prefs_.gpuIndex = gpu >= 0 ? std::optional<std::uint32_t>(gpu) : std::nullopt;
    prefs_.backend = backend_->currentData().toString();
    prefs_.validation = static_cast<Preferences::Validation>(validation_->currentIndex());
    prefs_.vsync = vsync_->isChecked();
    prefs_.projectsDirectory = location_->text().trimmed();
    prefs_.save();
    const bool restart = before.backend != prefs_.backend || before.gpuIndex != prefs_.gpuIndex || before.validation != prefs_.validation ||
                         before.vsync != prefs_.vsync;
    status_->setText(restart ? tr("Saved. Graphics changes take effect the next time Lumora starts.") : tr("Saved."));
}

} // namespace lumora::app
