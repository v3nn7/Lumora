#include "collab/CollabSession.hpp"

#include "collab/CollabClient.hpp"
#include "collab/Protocol.hpp"
#include "assets/FbxImporter.hpp"
#include "core/Log.hpp"
#include "scene/SceneSerializer.hpp"

#include <glm/geometric.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace lumora::collab {

namespace {
constexpr int kTickMs = 66;           // ~15 updates per second
constexpr qint64 kPoseKeepAliveMs = 1000;
}

CollabSession::CollabSession(CollabClient& client, Scene& scene, SceneSync::Applier applier, QString projectRoot,
                             QObject* parent)
    : QObject(parent), client_(client), sync_(scene, std::move(applier)), root_(std::move(projectRoot)) {
    timer_.setInterval(kTickMs);
    connect(&timer_, &QTimer::timeout, this, &CollabSession::tick);
    connect(&client_, &CollabClient::patchReceived, this, &CollabSession::onPatch);
    connect(&client_, &CollabClient::snapshotReceived, this, &CollabSession::onSnapshot);
    connect(&client_, &CollabClient::digestReceived, this, &CollabSession::onDigest);
    connect(&client_, &CollabClient::assetReceived, this, &CollabSession::onAssetReceived);
    connect(&client_, &CollabClient::assetUnavailable, this, [this](const QString& path) {
        requested_.remove(path);
        unavailable_.insert(path);
    });
    connect(&client_, &CollabClient::assetAvailable, this, [this](const QString& path) {
        if (unavailable_.remove(path) || !QFileInfo::exists(QDir(root_).filePath(path))) {
            requestMissing({path.toStdString()});
        }
        emit assetsArrived();  // the host may share our project folder (hosting)
    });
    connect(&client_, &CollabClient::left, this, [this] { timer_.stop(); });
}

void CollabSession::begin(const nlohmann::json& scene, quint64 seq) {
    const bool changed = sync_.replaceWith(scene);
    lastSeq_ = seq;
    requestMissing(referencedAssetFilesOfScene(scene));
    sincePose_.start();
    timer_.start();
    if (changed) emit remoteChange();
}

void CollabSession::tick() {
    if (client_.state() != CollabClient::State::Joined) return;
    if (auto patch = sync_.takeLocalChanges()) client_.sendPatch(*patch);
    sendPresence();
}

void CollabSession::sendPresence() {
    if (!poseSource) return;
    const Pose pose = poseSource();
    const bool moved = !posed_ || glm::distance(pose.position, lastPose_.position) > 1e-3f ||
                       glm::dot(pose.forward, lastPose_.forward) < 0.99999f || pose.selection != lastPose_.selection;
    if (!moved && sincePose_.elapsed() < kPoseKeepAliveMs) return;
    client_.sendPresence(pose.position, pose.forward, pose.selection);
    lastPose_ = pose;
    posed_ = true;
    sincePose_.restart();
}

void CollabSession::onPatch(const ScenePatch& patch, int from, quint64 seq) {
    const bool own = from == client_.id();
    if (!own) tick();  // our pending edits must be on their way before we merge others'
    sync_.receive(patch, own);
    lastSeq_ = seq;
    if (own) return;
    std::vector<std::string> files;
    for (const nlohmann::json& record : patch.upserts) {
        for (std::string& f : referencedAssetFiles(record)) files.push_back(std::move(f));
    }
    requestMissing(files);
    emit remoteChange();
}

void CollabSession::onSnapshot(const nlohmann::json& scene, quint64 seq) {
    resyncPending_ = false;
    tick();
    const bool changed = sync_.replaceWith(scene);
    lastSeq_ = seq;
    requestMissing(referencedAssetFilesOfScene(scene));
    if (changed) emit remoteChange();
}

void CollabSession::onDigest(quint64 seq, const QString& digest) {
    if (resyncPending_ || seq != lastSeq_ || !sync_.idle()) return;  // still catching up
    if (auto patch = sync_.takeLocalChanges()) {  // unsent edits: compare next time
        client_.sendPatch(*patch);
        return;
    }
    if (sceneDigest(SceneSerializer::toJson(sync_.scene())) == digest) return;
    log::warn("Collab", "scene drifted from the host (seq {}); reloading it", seq);
    resyncPending_ = true;
    emit resyncing();
    client_.requestResync();
}

void CollabSession::requestMissing(const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        const QString path = QString::fromStdString(file);
        if (!isSafeAssetPath(path) || requested_.contains(path)) continue;
        if (QFileInfo::exists(QDir(root_).filePath(path))) continue;
        requested_.insert(path);
        client_.requestAsset(path);
    }
}

void CollabSession::onAssetReceived(const QString& path) {
    requested_.remove(path);
    if (path.endsWith(QLatin1String(".gltf"), Qt::CaseInsensitive)) {
        // A .gltf loads its buffers and images from next to it: fetch those too.
        QFile file(QDir(root_).filePath(path));
        if (file.open(QIODevice::ReadOnly) && file.size() < (64 << 20)) {
            const QString folder = QFileInfo(path).path();
            std::vector<std::string> deps;
            for (const std::string& dep : gltfDependencies(file.readAll())) {
                deps.push_back(QDir::cleanPath(folder + QLatin1Char('/') + QString::fromStdString(dep)).toStdString());
            }
            requestMissing(deps);
        }
    } else if (const QString suffix = QFileInfo(path).suffix().toLower();
               suffix == QLatin1String("fbx") || suffix == QLatin1String("obj") || suffix == QLatin1String("mtl")) {
        // FBX and OBJ read textures (and an OBJ its .mtl, which names more) from next to them.
        const QString folder = QFileInfo(path).path();
        std::vector<std::string> deps;
        for (const std::filesystem::path& dep : fbxReferences(std::filesystem::path(QDir(root_).filePath(path).toStdU16String()))) {
            deps.push_back(QDir::cleanPath(folder + QLatin1Char('/') + QString::fromStdU16String(dep.generic_u16string())).toStdString());
        }
        requestMissing(deps);
    }
    emit assetsArrived();
}

} // namespace lumora::collab
