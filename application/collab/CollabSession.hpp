#pragma once

#include "common/ModuleExport.hpp"

#include "collab/SceneSync.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <glm/vec3.hpp>

#include <functional>

namespace lumora::collab {

class CollabClient;

/// Drives a joined session for one local scene: sends local edits and presence on a short
/// timer, applies others' edits, repairs drift when the host's digest disagrees, and fetches
/// asset files the scene refers to but the project folder lacks.
class LUMORA_COLLAB_API CollabSession final : public QObject {
    Q_OBJECT
public:
    struct Pose {
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
        QString selection;
    };

    CollabSession(CollabClient& client, Scene& scene, SceneSync::Applier applier, QString projectRoot,
                  QObject* parent = nullptr);

    /// Adopts the host's scene (from CollabClient::joined) and starts the update timer.
    void begin(const nlohmann::json& scene, quint64 seq);
    /// Sends pending local edits and presence now (the timer calls this; tests call it directly).
    void tick();

    std::function<Pose()> poseSource;  // camera + selection, sampled every tick

    [[nodiscard]] SceneSync& sync() noexcept { return sync_; }
    [[nodiscard]] quint64 lastSequence() const noexcept { return lastSeq_; }

signals:
    /// The scene was changed by someone else (refresh views, mark the scene modified).
    void remoteChange();
    /// Files arrived in the project folder (retry assets that failed to load).
    void assetsArrived();
    /// Drift was detected and the scene is being reloaded from the host.
    void resyncing();

private:
    void onPatch(const ScenePatch& patch, int from, quint64 seq);
    void onSnapshot(const nlohmann::json& scene, quint64 seq);
    void onDigest(quint64 seq, const QString& digest);
    void onAssetReceived(const QString& path);
    void requestMissing(const std::vector<std::string>& files);
    void sendPresence();

    CollabClient& client_;
    SceneSync sync_;
    QString root_;
    quint64 lastSeq_ = 0;
    QTimer timer_;
    QSet<QString> requested_;
    QSet<QString> unavailable_;
    bool resyncPending_ = false;
    Pose lastPose_;
    bool posed_ = false;
    QElapsedTimer sincePose_;
};

} // namespace lumora::collab
