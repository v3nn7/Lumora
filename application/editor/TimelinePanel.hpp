#pragma once

#include "common/ModuleExport.hpp"
#include "scene/Timeline.hpp"

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

#include <QWidget>

#include <optional>
#include <unordered_map>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lumora::editor {

class EditorContext;

/// Edits a timeline (.ltimeline): tracks per target entity with keys over time. Key the
/// selected entity's transform or visibility at the playhead, add animation, sound and event
/// keys, scrub and play it back. With Preview on, the scene shows the timeline at the playhead;
/// turning it off puts every target back where it was.
class LUMORA_EDITOR_API TimelinePanel final : public QWidget {
    Q_OBJECT
public:
    TimelinePanel(EditorContext& ctx, QString projectRoot, QWidget* parent = nullptr);
    ~TimelinePanel() override;

    bool open(const QString& file);
    bool save();
    [[nodiscard]] const QString& file() const noexcept { return file_; }
    [[nodiscard]] const TimelineAsset& timeline() const noexcept { return timeline_; }
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }

    /// Playhead in seconds.
    void setTime(float seconds);
    [[nodiscard]] float time() const noexcept { return time_; }
    void setPreview(bool on);
    [[nodiscard]] bool previewing() const noexcept { return previewing_; }

    // Editing (also the buttons' actions); the selected entity is the target.
    bool keySelectedTransform();
    bool keySelectedVisibility();
    bool addEvent(const QString& name);
    bool addAnimationKey(const QString& clip);
    bool addSoundKey();
    /// Removes the selected key or track.
    void removeSelected();

private:
    void rebuildTree();
    void applyPreview();
    void restoreScene();
    void markDirty();
    [[nodiscard]] entt::entity selectedEntity() const;
    [[nodiscard]] entt::entity targetEntity(const std::string& name) const;
    TimelineAsset::Track* trackForSelection(TimelineAsset::TrackKind kind, entt::entity& entity);

    EditorContext& ctx_;
    QString root_;
    QString file_;
    TimelineAsset timeline_;
    bool dirty_ = false;
    float time_ = 0.0f;
    bool previewing_ = false;
    struct Saved {
        glm::vec3 position;
        glm::vec3 rotation;
        std::optional<bool> visible;
    };
    std::unordered_map<entt::entity, Saved> saved_;  // targets as they were before the preview

    QLabel* title_ = nullptr;
    QDoubleSpinBox* duration_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QCheckBox* preview_ = nullptr;
    QToolButton* play_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTimer* player_ = nullptr;
};

} // namespace lumora::editor
