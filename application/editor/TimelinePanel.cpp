#include "editor/TimelinePanel.hpp"

#include "common/ProjectUi.hpp"
#include "common/Theme.hpp"
#include "editor/EditorContext.hpp"
#include "scene/Scene.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace lumora::editor {

namespace {
constexpr int kSliderSteps = 1000;
using Kind = TimelineAsset::TrackKind;

QString kindLabel(Kind kind) {
    switch (kind) {
        case Kind::Transform: return QObject::tr("Transform");
        case Kind::Visible: return QObject::tr("Visibility");
        case Kind::Animation: return QObject::tr("Animation");
        case Kind::Sound: return QObject::tr("Sound");
        case Kind::Event: return QObject::tr("Events");
    }
    return {};
}
} // namespace

TimelinePanel::TimelinePanel(EditorContext& ctx, QString projectRoot, QWidget* parent)
    : QWidget(parent), ctx_(ctx), root_(std::move(projectRoot)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* bar = new QHBoxLayout;
    title_ = new QLabel(tr("No timeline: create one with New → Timeline in the Content Browser"));
    title_->setProperty("muted", true);
    auto* saveButton = new QToolButton;
    saveButton->setIcon(app::theme::icon(QStringLiteral("save")));
    saveButton->setToolTip(tr("Save the timeline"));
    duration_ = new QDoubleSpinBox;
    duration_->setRange(0.1, 3600.0);
    duration_->setSuffix(tr(" s"));
    duration_->setDecimals(2);
    duration_->setToolTip(tr("Length of the timeline"));
    preview_ = new QCheckBox(tr("Preview"));
    preview_->setToolTip(tr("Show the timeline in the scene at the playhead (off: everything goes back)"));
    play_ = new QToolButton;
    play_->setIcon(app::theme::icon(QStringLiteral("play")));
    play_->setToolTip(tr("Play the preview"));
    play_->setCheckable(true);
    bar->addWidget(title_, 1);
    bar->addWidget(new QLabel(tr("Length")));
    bar->addWidget(duration_);
    bar->addWidget(preview_);
    bar->addWidget(play_);
    bar->addWidget(saveButton);
    layout->addLayout(bar);

    auto* scrub = new QHBoxLayout;
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(0, kSliderSteps);
    timeLabel_ = new QLabel(QStringLiteral("0.00 s"));
    timeLabel_->setMinimumWidth(70);
    scrub->addWidget(slider_, 1);
    scrub->addWidget(timeLabel_);
    layout->addLayout(scrub);

    auto* buttons = new QHBoxLayout;
    auto button = [&](const QString& text, const QString& tip, auto&& fn) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        buttons->addWidget(b);
    };
    button(tr("Key Transform"), tr("Key the selected entity's position and rotation at the playhead"), [this] {
        if (!keySelectedTransform()) QMessageBox::information(this, tr("Timeline"), tr("Select an entity in the scene first."));
    });
    button(tr("Key Visibility"), tr("Key whether the selected entity's mesh shows"), [this] { keySelectedVisibility(); });
    button(tr("Animation…"), tr("Start an animation clip of the selected entity here"), [this] {
        bool ok = false;
        const QString clip = QInputDialog::getText(this, tr("Animation Key"), tr("Clip name"), QLineEdit::Normal, {}, &ok);
        if (ok) addAnimationKey(clip);
    });
    button(tr("Sound"), tr("Play the selected entity's Audio Source here"), [this] { addSoundKey(); });
    button(tr("Event…"), tr("Send an event scripts see with timelineEvent(name)"), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Event Key"), tr("Event name"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) addEvent(name.trimmed());
    });
    button(tr("Delete"), tr("Delete the selected key or track"), [this] { removeSelected(); });
    buttons->addStretch(1);
    layout->addLayout(buttons);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Track / key"), tr("Value")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);

    player_ = new QTimer(this);
    player_->setInterval(16);
    auto* clock = new QElapsedTimer;
    connect(player_, &QTimer::timeout, this, [this, clock] {
        const float dt = static_cast<float>(clock->restart()) / 1000.0f;
        float t = time_ + dt;
        if (t >= timeline_.duration) t = 0.0f;  // loops while previewing
        setTime(t);
    });
    connect(this, &QObject::destroyed, [clock] { delete clock; });
    connect(play_, &QToolButton::toggled, this, [this, clock](bool on) {
        if (on) {
            setPreview(true);
            clock->restart();
            player_->start();
        } else {
            player_->stop();
        }
        play_->setIcon(app::theme::icon(on ? QStringLiteral("pause") : QStringLiteral("play")));
    });
    connect(slider_, &QSlider::valueChanged, this, [this](int v) {
        setTime(static_cast<float>(v) / kSliderSteps * timeline_.duration);
    });
    connect(preview_, &QCheckBox::toggled, this, [this](bool on) { setPreview(on); });
    connect(duration_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (std::abs(timeline_.duration - static_cast<float>(v)) < 1e-4f) return;
        timeline_.duration = static_cast<float>(v);
        markDirty();
        setTime(std::min(time_, timeline_.duration));
    });
    connect(saveButton, &QToolButton::clicked, this, [this] { save(); });
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const QVariant t = item->data(0, Qt::UserRole + 1);
        if (t.isValid()) setTime(t.toFloat());
    });
    connect(&ctx_, &EditorContext::sceneReplaced, this, [this] {
        saved_.clear();  // a new scene: nothing to put back
        previewing_ = false;
        const QSignalBlocker block(preview_);
        preview_->setChecked(false);
    });
    setEnabled(true);
}

TimelinePanel::~TimelinePanel() { restoreScene(); }

bool TimelinePanel::open(const QString& file) {
    auto loaded = loadTimeline(app::toPath(file));
    if (!loaded) {
        QMessageBox::warning(this, tr("Timeline"), QString::fromStdString(loaded.error().message));
        return false;
    }
    setPreview(false);
    file_ = file;
    timeline_ = std::move(*loaded);
    dirty_ = false;
    {
        const QSignalBlocker block(duration_);
        duration_->setValue(timeline_.duration);
    }
    title_->setProperty("muted", false);
    rebuildTree();
    setTime(0.0f);
    return true;
}

bool TimelinePanel::save() {
    if (file_.isEmpty()) return false;
    if (auto saved = saveTimeline(timeline_, app::toPath(file_)); !saved) {
        QMessageBox::warning(this, tr("Timeline"), QString::fromStdString(saved.error().message));
        return false;
    }
    dirty_ = false;
    rebuildTree();
    return true;
}

void TimelinePanel::markDirty() {
    dirty_ = true;
    rebuildTree();
    if (previewing_) applyPreview();
}

void TimelinePanel::setTime(float seconds) {
    time_ = std::clamp(seconds, 0.0f, timeline_.duration);
    {
        const QSignalBlocker block(slider_);
        slider_->setValue(static_cast<int>(std::lround(time_ / std::max(timeline_.duration, 0.01f) * kSliderSteps)));
    }
    timeLabel_->setText(QStringLiteral("%1 s").arg(time_, 0, 'f', 2));
    if (previewing_) applyPreview();
}

entt::entity TimelinePanel::selectedEntity() const {
    Scene* scene = ctx_.scene();
    if (scene == nullptr || ctx_.selection().empty()) return entt::null;
    return scene->findByUuid(ctx_.selection().front());
}

entt::entity TimelinePanel::targetEntity(const std::string& name) const {
    Scene* scene = ctx_.scene();
    if (scene == nullptr) return entt::null;
    for (auto e : scene->registry().view<TransformComponent>()) {
        if (scene->nameOf(e) == name) return e;
    }
    return entt::null;
}

TimelineAsset::Track* TimelinePanel::trackForSelection(Kind kind, entt::entity& entity) {
    entity = selectedEntity();
    if (entity == entt::null || file_.isEmpty()) return nullptr;
    return &timeline_.track(kind, ctx_.scene()->nameOf(entity));
}

void TimelinePanel::setPreview(bool on) {
    if (on == previewing_) return;
    if (on) {
        previewing_ = true;
        applyPreview();
    } else {
        play_->setChecked(false);
        restoreScene();
        previewing_ = false;
    }
    const QSignalBlocker block(preview_);
    preview_->setChecked(on);
}

void TimelinePanel::applyPreview() {
    Scene* scene = ctx_.scene();
    if (scene == nullptr) return;
    auto& reg = scene->registry();
    for (const auto& track : timeline_.tracks) {
        if (track.kind != Kind::Transform && track.kind != Kind::Visible) continue;
        const entt::entity e = targetEntity(track.target);
        if (e == entt::null) continue;
        if (!saved_.contains(e)) {
            const auto& t = reg.get<TransformComponent>(e);
            const auto* mesh = reg.try_get<MeshRendererComponent>(e);
            saved_[e] = {t.position, t.rotationDeg, mesh != nullptr ? std::optional<bool>(mesh->visible) : std::nullopt};
        }
        if (track.kind == Kind::Transform) {
            if (const auto pose = evaluateTransform(track, time_)) {
                auto& t = reg.get<TransformComponent>(e);
                t.position = pose->position;
                t.rotationDeg = pose->rotationDeg;
            }
        } else if (auto* mesh = reg.try_get<MeshRendererComponent>(e)) {
            mesh->visible = evaluateVisible(track, time_).value_or(saved_[e].visible.value_or(true));
        }
    }
    scene->updateWorldTransforms();
    emit ctx_.valuesChanged(this);
}

void TimelinePanel::restoreScene() {
    Scene* scene = ctx_.scene();
    if (scene == nullptr || saved_.empty()) {
        saved_.clear();
        return;
    }
    auto& reg = scene->registry();
    for (const auto& [e, s] : saved_) {
        if (!reg.valid(e)) continue;
        auto& t = reg.get<TransformComponent>(e);
        t.position = s.position;
        t.rotationDeg = s.rotation;
        if (auto* mesh = reg.try_get<MeshRendererComponent>(e); mesh != nullptr && s.visible) mesh->visible = *s.visible;
    }
    saved_.clear();
    scene->updateWorldTransforms();
    emit ctx_.valuesChanged(this);
}

bool TimelinePanel::keySelectedTransform() {
    entt::entity e = entt::null;
    TimelineAsset::Track* track = trackForSelection(Kind::Transform, e);
    if (track == nullptr) return false;
    const auto& t = ctx_.scene()->registry().get<TransformComponent>(e);
    TimelineAsset::Key key;
    key.time = time_;
    key.position = t.position;
    key.rotationDeg = t.rotationDeg;
    TimelineAsset::setKey(*track, key);
    // The pose being keyed is the one the user set up: it stays after the preview, too.
    if (auto it = saved_.find(e); it != saved_.end() && track->keys.size() == 1) saved_.erase(it);
    markDirty();
    return true;
}

bool TimelinePanel::keySelectedVisibility() {
    entt::entity e = entt::null;
    TimelineAsset::Track* track = trackForSelection(Kind::Visible, e);
    if (track == nullptr) return false;
    const auto* mesh = ctx_.scene()->registry().try_get<MeshRendererComponent>(e);
    TimelineAsset::Key key;
    key.time = time_;
    key.visible = mesh == nullptr || mesh->visible;
    TimelineAsset::setKey(*track, key);
    markDirty();
    return true;
}

bool TimelinePanel::addAnimationKey(const QString& clip) {
    entt::entity e = entt::null;
    TimelineAsset::Track* track = trackForSelection(Kind::Animation, e);
    if (track == nullptr) return false;
    TimelineAsset::Key key;
    key.time = time_;
    key.text = clip.toStdString();
    TimelineAsset::setKey(*track, key);
    markDirty();
    return true;
}

bool TimelinePanel::addSoundKey() {
    entt::entity e = entt::null;
    TimelineAsset::Track* track = trackForSelection(Kind::Sound, e);
    if (track == nullptr) return false;
    TimelineAsset::Key key;
    key.time = time_;
    TimelineAsset::setKey(*track, key);
    markDirty();
    return true;
}

bool TimelinePanel::addEvent(const QString& name) {
    if (file_.isEmpty()) return false;
    TimelineAsset::Key key;
    key.time = time_;
    key.text = name.toStdString();
    TimelineAsset::setKey(timeline_.track(Kind::Event, {}), key);
    markDirty();
    return true;
}

void TimelinePanel::removeSelected() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return;
    const int trackIndex = item->data(0, Qt::UserRole).toInt();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(timeline_.tracks.size())) return;
    auto& track = timeline_.tracks[static_cast<std::size_t>(trackIndex)];
    const QVariant keyIndex = item->data(1, Qt::UserRole);
    if (keyIndex.isValid()) {
        track.keys.erase(track.keys.begin() + keyIndex.toInt());
    } else {
        timeline_.tracks.erase(timeline_.tracks.begin() + trackIndex);
    }
    markDirty();
}

void TimelinePanel::rebuildTree() {
    title_->setText(file_.isEmpty() ? tr("No timeline")
                                    : QFileInfo(file_).fileName() + (dirty_ ? QStringLiteral(" *") : QString()));
    tree_->clear();
    for (std::size_t i = 0; i < timeline_.tracks.size(); ++i) {
        const auto& track = timeline_.tracks[i];
        auto* row = new QTreeWidgetItem(tree_);
        const QString target = track.target.empty() ? tr("(timeline)") : QString::fromStdString(track.target);
        row->setText(0, track.kind == Kind::Event ? kindLabel(track.kind) : target + QStringLiteral(" · ") + kindLabel(track.kind));
        row->setText(1, tr("%n key(s)", nullptr, static_cast<int>(track.keys.size())));
        row->setData(0, Qt::UserRole, static_cast<int>(i));
        for (std::size_t k = 0; k < track.keys.size(); ++k) {
            const auto& key = track.keys[k];
            auto* child = new QTreeWidgetItem(row);
            child->setText(0, QStringLiteral("%1 s").arg(key.time, 0, 'f', 2));
            QString value;
            switch (track.kind) {
                case Kind::Transform:
                    value = QStringLiteral("(%1, %2, %3)").arg(key.position.x, 0, 'f', 2).arg(key.position.y, 0, 'f', 2).arg(key.position.z, 0, 'f', 2);
                    break;
                case Kind::Visible: value = key.visible ? tr("shown") : tr("hidden"); break;
                case Kind::Animation:
                case Kind::Event: value = QString::fromStdString(key.text); break;
                case Kind::Sound: value = tr("play"); break;
            }
            child->setText(1, value);
            child->setData(0, Qt::UserRole, static_cast<int>(i));
            child->setData(1, Qt::UserRole, static_cast<int>(k));
            child->setData(0, Qt::UserRole + 1, key.time);
        }
        row->setExpanded(true);
    }
}

} // namespace lumora::editor
