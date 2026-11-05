#pragma once

#include "collab/Protocol.hpp"
#include "core/Error.hpp"

#include <QHostAddress>
#include <QObject>
#include <QTimer>

#include <map>
#include <memory>
#include <vector>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

namespace lumora {
class Scene;
}

namespace lumora::collab {

/// Hosts a live editing session: listens on one port for TCP (scene changes, chat, asset
/// files) and UDP (presence), keeps the authoritative copy of the shared scene and orders all
/// changes. Each change is applied in arrival order (last writer wins per entity) and echoed to
/// every client, including its author, as the acknowledgement. A digest of the scene goes out
/// every few seconds so clients can detect and repair drift.
///
/// Assets: the host's project folder is the shared store. A client missing a file asks for it;
/// when a client's change refers to a file the host lacks, the host fetches it from that client
/// and tells everyone once it has arrived.
class CollabServer final : public QObject {
    Q_OBJECT
public:
    struct Options {
        quint16 port = kDefaultPort;  // 0 = any free port (tests)
        QHostAddress bindAddress = QHostAddress::Any;
        QString password;             // empty = none
        QString sessionName;
        QString projectName;
        QString projectRoot;          // asset store (the host's project folder)
        int maxClients = 16;
    };

    explicit CollabServer(QObject* parent = nullptr);
    ~CollabServer() override;

    Result<> start(const Options& options, const nlohmann::json& sceneDocument);
    void stop();
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] quint16 port() const noexcept;
    [[nodiscard]] bool udpAvailable() const noexcept;
    [[nodiscard]] int clientCount() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept { return seq_; }
    [[nodiscard]] nlohmann::json document() const;

signals:
    void clientsChanged();

private:
    struct Peer;

    void onNewConnection();
    void onReadable(Peer& peer);
    void onDatagrams();
    void handle(Peer& peer, const Frame& frame);
    void admit(Peer& peer, const nlohmann::json& hello);
    void applyPatch(Peer& peer, const nlohmann::json& message);
    void relayPresence(const Peer& from);
    void fetchMissingAssets(Peer& author, const std::vector<std::string>& files);
    void sendDigest();
    void send(Peer& peer, const nlohmann::json& message);
    void broadcast(const nlohmann::json& message, const Peer* except = nullptr);
    void drop(Peer& peer, const QString& reason);
    void remove(QTcpSocket* socket);
    [[nodiscard]] Peer* findPeer(int id);
    [[nodiscard]] nlohmann::json userJson(const Peer& peer) const;

    Options options_;
    std::unique_ptr<QTcpServer> tcp_;
    std::unique_ptr<QUdpSocket> udp_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<std::unique_ptr<Peer>> retired_;  // removed during a handler; freed on the next loop pass
    std::unique_ptr<Scene> document_;
    std::uint64_t seq_ = 0;
    int nextId_ = 1;
    std::map<QString, int> fetching_;  // asset path -> id of the client it is fetched from
    QTimer digestTimer_;
};

} // namespace lumora::collab
