#pragma once

#include "common/ModuleExport.hpp"

#include "collab/Protocol.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QTimer>

#include <glm/vec3.hpp>

#include <map>
#include <memory>
#include <optional>

class QSslSocket;
class QSslError;
class QUdpSocket;

namespace lumora {
struct ScenePatch;
}

namespace lumora::collab {

class AssetReceiver;
class AssetSender;

struct Participant {
    int id = 0;
    QString name;
    QColor color;
    // Presence (camera and selection), when known.
    bool hasPose = false;
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    QString selection;  // entity uuid, empty = nothing
};

/// One participant's connection to a session: TLS to the host, presence over sealed UDP (falling
/// back to the TLS connection when datagrams do not get through), and asset files in both
/// directions. Scene changes are passed through as patches; SceneSync decides how they meet the
/// local scene.
///
/// The host's certificate is self-signed, so it is checked by fingerprint: the one pinned in
/// the address, else the one remembered for that host (KnownHosts). An unknown host is not
/// connected to - untrustedHost() reports its fingerprint so the user can confirm it and retry.
class LUMORA_COLLAB_API CollabClient final : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Joined };

    explicit CollabClient(QObject* parent = nullptr);
    ~CollabClient() override;

    void connectTo(const Address& address, const QString& name, const QString& password);
    void disconnectFromHost();
    /// Folder that downloaded assets go to and that the host may fetch assets from.
    void setProjectRoot(const QString& root);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] int id() const noexcept { return id_; }
    [[nodiscard]] const Address& address() const noexcept { return address_; }
    [[nodiscard]] const QString& sessionName() const noexcept { return session_; }
    [[nodiscard]] const QString& projectName() const noexcept { return project_; }
    [[nodiscard]] const std::map<int, Participant>& participants() const noexcept { return participants_; }
    [[nodiscard]] bool udpActive() const noexcept;
    [[nodiscard]] std::optional<int> latencyMs() const noexcept { return latency_; }
    /// Host certificate fingerprint and negotiated protocol of the current connection.
    [[nodiscard]] const QByteArray& hostFingerprint() const noexcept { return hostFingerprint_; }
    [[nodiscard]] QString encryption() const;

    void sendPatch(const ScenePatch& patch);
    void sendPresence(const glm::vec3& position, const glm::vec3& forward, const QString& selection);
    void sendChat(const QString& text);
    void requestAsset(const QString& path);
    void requestResync();

signals:
    void joined(const nlohmann::json& scene, quint64 seq);
    /// Connecting failed or the host refused (wrong password, version, full).
    void joinFailed(const QString& reason);
    /// The host's certificate is neither pinned nor known; emitted before joinFailed.
    void untrustedHost(const QString& host, quint16 port, const QByteArray& fingerprint);
    /// A joined session ended (host left, network lost, or disconnectFromHost()).
    void left(const QString& reason);
    void patchReceived(const lumora::ScenePatch& patch, int from, quint64 seq);
    void snapshotReceived(const nlohmann::json& scene, quint64 seq);
    void digestReceived(quint64 seq, const QString& digest);
    void participantsChanged();
    void presenceChanged(int id);
    void chatReceived(int from, const QString& text);
    void assetReceived(const QString& path);     // written into the project folder
    void assetUnavailable(const QString& path);  // the host does not have it (yet)
    void assetAvailable(const QString& path);    // the host has it now
    void connectionInfoChanged();                // UDP status or latency

private:
    void onReadable();
    void onSslErrors(const QList<QSslError>& errors);
    void onDatagrams();
    void handle(const Frame& frame);
    void onWelcome(const nlohmann::json& m);
    void updatePresence(const nlohmann::json& m);
    void send(const nlohmann::json& message);
    void sendDatagram(const nlohmann::json& message);
    void heartbeat();
    void finish(const QString& reason);

    std::unique_ptr<QSslSocket> socket_;
    std::unique_ptr<QUdpSocket> udp_;
    std::unique_ptr<AssetSender> sender_;
    std::unique_ptr<AssetReceiver> receiver_;
    FrameReader reader_;
    State state_ = State::Disconnected;
    Address address_;
    QString name_;
    QString password_;
    QString root_;
    QString session_;
    QString project_;
    int id_ = 0;
    QByteArray udpKey_;
    QByteArray hostFingerprint_;
    bool hostUdp_ = false;
    QHostAddress udpTarget_;
    QElapsedTimer sinceUdpAck_;
    bool udpReported_ = false;
    std::optional<int> latency_;
    QElapsedTimer clock_;
    QTimer heartbeat_;
    std::map<int, Participant> participants_;
};

} // namespace lumora::collab
