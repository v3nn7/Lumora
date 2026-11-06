#include "collab/CollabServer.hpp"

#include "collab/AssetTransfer.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"
#include "scene/ScenePatch.hpp"
#include "scene/SceneSerializer.hpp"

#include <QDir>
#include <QFileInfo>
#include <QNetworkDatagram>
#include <QPointer>
#include <QRandomGenerator>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpSocket>
#include <QUdpSocket>

#include <algorithm>

namespace lumora::collab {

using nlohmann::json;

namespace {

constexpr std::uint32_t kPreAdmissionLimit = 64u << 10;
constexpr int kHelloTimeoutMs = 10000;
constexpr int kDigestIntervalMs = 3000;

const char* const kPalette[] = {"#f2a03d", "#4fb3ff", "#7bd88f", "#ff6b9a", "#c792ea", "#ffd166", "#5ce1e6", "#ff8c66"};

json presenceFields(const json& message) {
    json out = json::object();
    for (const char* key : {"p", "f", "s"}) {
        if (const auto it = message.find(key); it != message.end()) out[key] = *it;
    }
    return out;
}

bool validPresence(const json& p) {
    auto vec3 = [](const json& v) {
        return v.is_array() && v.size() == 3 && std::ranges::all_of(v, [](const json& c) { return c.is_number(); });
    };
    if (p.contains("p") && !vec3(p["p"])) return false;
    if (p.contains("f") && !vec3(p["f"])) return false;
    if (p.contains("s") && !p["s"].is_string()) return false;
    return true;
}

} // namespace

struct CollabServer::Peer {
    QTcpSocket* socket = nullptr;
    FrameReader reader;
    std::unique_ptr<AssetSender> sender;
    std::unique_ptr<AssetReceiver> receiver;
    int id = 0;  // 0 until admitted
    QString name;
    QString color;
    QByteArray udpKey;  // seals this participant's presence datagrams (sent over TLS)
    bool udp = false;  // presence is delivered over UDP
    QHostAddress udpHost;
    quint16 udpPort = 0;
    json presence = json::object();
    bool gone = false;  // removed; only kept alive until the current handler returns
};

CollabServer::CollabServer(QObject* parent) : QObject(parent) {
    digestTimer_.setInterval(kDigestIntervalMs);
    connect(&digestTimer_, &QTimer::timeout, this, &CollabServer::sendDigest);
}

CollabServer::~CollabServer() { stop(); }

Result<> CollabServer::start(const Options& options, const json& sceneDocument) {
    stop();
    auto scene = SceneSerializer::fromJson(sceneDocument);
    if (!scene) return std::unexpected(scene.error());
    options_ = options;
    document_ = std::move(*scene);
    seq_ = 0;

    if (!QSslSocket::supportsSsl()) {
        return makeError("TLS is not available: Qt's OpenSSL backend (or OpenSSL itself) is missing");
    }
    if (!options.identity.isValid()) return makeError("the session needs a host certificate");
    tcp_ = std::make_unique<QSslServer>();
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setLocalCertificate(options.identity.certificate);
    tls.setPrivateKey(options.identity.key);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);  // guests authenticate with the session password
    tcp_->setSslConfiguration(tls);
    tcp_->setHandshakeTimeout(kHelloTimeoutMs);
    if (!tcp_->listen(options.bindAddress, options.port)) {
        const QString why = tcp_->errorString();
        tcp_.reset();
        return makeError("cannot listen on TCP port {}: {}", options.port, why.toStdString());
    }
    tcp_->setMaxPendingConnections(8);
    connect(tcp_.get(), &QTcpServer::pendingConnectionAvailable, this, &CollabServer::onNewConnection);
    // Before the TLS handshake ends nobody is admitted yet: log it, so a tunnel that reaches the
    // host (or one that does not) shows up in the host's log.
    connect(tcp_.get(), &QSslServer::startedEncryptionHandshake, this, [](QSslSocket* socket) {
        log::info("Collab", "connection from {} (TLS handshake)", socket->peerAddress().toString().toStdString());
    });
    connect(tcp_.get(), &QSslServer::errorOccurred, this, [](QSslSocket* socket, QAbstractSocket::SocketError) {
        log::warn("Collab", "connection from {} failed before joining: {}", socket->peerAddress().toString().toStdString(),
                  socket->errorString().toStdString());
    });

    udp_ = std::make_unique<QUdpSocket>();
    if (udp_->bind(options.bindAddress, tcp_->serverPort())) {
        connect(udp_.get(), &QUdpSocket::readyRead, this, &CollabServer::onDatagrams);
    } else {
        log::warn("Collab", "UDP port {} unavailable ({}); presence goes over TCP", tcp_->serverPort(),
                  udp_->errorString().toStdString());
        udp_.reset();
    }
    digestTimer_.start();
    log::info("Collab", "hosting '{}' on port {} (TLS, certificate {})", options.sessionName.toStdString(),
              tcp_->serverPort(), formatFingerprint(options.identity.fingerprint, 4).toStdString());
    return {};
}

void CollabServer::stop() {
    digestTimer_.stop();
    while (!peers_.empty()) {
        QTcpSocket* socket = peers_.back()->socket;
        peers_.pop_back();
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    fetching_.clear();
    retired_.clear();
    if (tcp_) tcp_->close();
    tcp_.reset();
    udp_.reset();
    document_.reset();
}

bool CollabServer::isRunning() const noexcept { return tcp_ && tcp_->isListening(); }
quint16 CollabServer::port() const noexcept { return tcp_ ? tcp_->serverPort() : 0; }
bool CollabServer::udpAvailable() const noexcept { return udp_ != nullptr; }

int CollabServer::clientCount() const noexcept {
    return static_cast<int>(std::ranges::count_if(peers_, [](const auto& p) { return p->id != 0; }));
}

json CollabServer::document() const { return document_ ? SceneSerializer::toJson(*document_) : json(); }

CollabServer::Peer* CollabServer::findPeer(int id) {
    for (auto& p : peers_) {
        if (p->id == id) return p.get();
    }
    return nullptr;
}

json CollabServer::userJson(const Peer& peer) const {
    return {{"id", peer.id}, {"name", peer.name.toStdString()}, {"color", peer.color.toStdString()}};
}

void CollabServer::send(Peer& peer, const json& message) { peer.socket->write(encodeMessage(message)); }

void CollabServer::broadcast(const json& message, const Peer* except) {
    const QByteArray bytes = encodeMessage(message);
    for (auto& p : peers_) {
        if (p->id != 0 && p.get() != except) p->socket->write(bytes);
    }
}

void CollabServer::onNewConnection() {
    while (tcp_ && tcp_->hasPendingConnections()) {
        QTcpSocket* socket = tcp_->nextPendingConnection();
        if (static_cast<int>(peers_.size()) >= options_.maxClients) {
            socket->write(encodeMessage({{"t", "reject"}, {"reason", "The session is full."}}));
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        auto peer = std::make_unique<Peer>();
        peer->socket = socket;
        peer->reader.setJsonLimit(kPreAdmissionLimit);
        peer->sender = std::make_unique<AssetSender>(*socket, options_.projectRoot);
        peer->receiver = std::make_unique<AssetReceiver>(options_.projectRoot);
        Peer* raw = peer.get();
        peers_.push_back(std::move(peer));
        connect(socket, &QTcpSocket::readyRead, this, [this, raw] { onReadable(*raw); });
        connect(socket, &QTcpSocket::bytesWritten, this, [raw] { raw->sender->pump(); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { remove(socket); });
        QPointer<QTcpSocket> guard(socket);
        QTimer::singleShot(kHelloTimeoutMs, this, [this, guard] {
            if (!guard) return;
            for (auto& p : peers_) {
                if (p->socket == guard && p->id == 0) drop(*p, QStringLiteral("no hello"));
            }
        });
    }
}

void CollabServer::onReadable(Peer& peer) {
    peer.reader.append(peer.socket->readAll());
    while (auto frame = peer.reader.next()) {
        handle(peer, *frame);
        if (peer.gone || peer.socket->state() != QAbstractSocket::ConnectedState) return;
    }
    if (peer.reader.failed()) drop(peer, peer.reader.error());
}

void CollabServer::drop(Peer& peer, const QString& reason) {
    log::warn("Collab", "dropping client {} ({}): {}", peer.id, peer.name.toStdString(), reason.toStdString());
    peer.socket->abort();  // emits disconnected -> remove()
}

void CollabServer::remove(QTcpSocket* socket) {
    const auto it = std::ranges::find_if(peers_, [socket](const auto& p) { return p->socket == socket; });
    if (it == peers_.end()) return;
    std::unique_ptr<Peer> peer = std::move(*it);
    peers_.erase(it);
    peer->gone = true;
    socket->deleteLater();
    std::erase_if(fetching_, [&](const auto& entry) { return entry.second == peer->id; });
    if (peer->id != 0) {
        log::info("Collab", "{} left", peer->name.toStdString());
        broadcast({{"t", "userLeft"}, {"id", peer->id}});
    }
    const bool admitted = peer->id != 0;
    retired_.push_back(std::move(peer));  // the caller may still hold a reference to it
    QTimer::singleShot(0, this, [this] { retired_.clear(); });
    if (admitted) emit clientsChanged();
}

void CollabServer::handle(Peer& peer, const Frame& frame) {
    if (frame.kind == FrameKind::AssetChunk) {
        if (peer.id == 0) return drop(peer, QStringLiteral("data before hello"));
        QString error;
        switch (peer.receiver->accept(frame.chunk, &error)) {
            case AssetReceiver::Outcome::Progress:
                break;
            case AssetReceiver::Outcome::Completed:
                fetching_.erase(frame.chunk.path);
                log::info("Collab", "received {} from {}", frame.chunk.path.toStdString(), peer.name.toStdString());
                broadcast({{"t", "assetAvailable"}, {"path", frame.chunk.path.toStdString()}});
                break;
            case AssetReceiver::Outcome::Rejected:
                fetching_.erase(frame.chunk.path);
                log::warn("Collab", "upload rejected: {}", error.toStdString());
                break;
        }
        return;
    }

    const json& m = frame.message;
    const std::string type = m["t"].get<std::string>();
    if (peer.id == 0) {
        if (type != "hello") return drop(peer, QStringLiteral("expected hello"));
        return admit(peer, m);
    }
    if (type == "patch") {
        applyPatch(peer, m);
    } else if (type == "presence") {
        if (!validPresence(m)) return;
        peer.presence = presenceFields(m);
        relayPresence(peer);
    } else if (type == "presenceMode") {
        peer.udp = m.value("udp", false) && peer.udpPort != 0;
    } else if (type == "assetGet") {
        const QString path = QString::fromStdString(m.value("path", std::string{}));
        if (fetching_.contains(path) || !peer.sender->enqueue(path)) {
            send(peer, {{"t", "assetMissing"}, {"path", path.toStdString()}});
        }
    } else if (type == "assetMissing") {  // reply to our own request
        const QString path = QString::fromStdString(m.value("path", std::string{}));
        if (fetching_.contains(path) && fetching_[path] == peer.id) {
            fetching_.erase(path);
            peer.receiver->cancel(path);
        }
    } else if (type == "chat") {
        const QString text = QString::fromStdString(m.value("text", std::string{})).left(kMaxChatLength).trimmed();
        if (!text.isEmpty()) broadcast({{"t", "chat"}, {"from", peer.id}, {"text", text.toStdString()}});
    } else if (type == "resync") {
        send(peer, {{"t", "snapshot"}, {"seq", seq_}, {"scene", SceneSerializer::toJson(*document_)}});
    } else if (type == "ping") {
        send(peer, {{"t", "pong"}, {"time", m.value("time", 0.0)}});
    } else {
        log::debug("Collab", "ignoring message '{}'", type);
    }
}

void CollabServer::admit(Peer& peer, const json& hello) {
    auto reject = [&](const std::string& reason) {
        send(peer, {{"t", "reject"}, {"reason", reason}});
        peer.socket->disconnectFromHost();
    };
    if (hello.value("v", 0) != kProtocolVersion) {
        return reject("This session uses a different Lumora version (protocol " + std::to_string(kProtocolVersion) + ").");
    }
    if (!options_.password.isEmpty() &&
        QString::fromStdString(hello.value("password", std::string{})) != options_.password) {
        return reject("Wrong session password.");
    }
    peer.id = nextId_++;
    peer.name = sanitizeName(QString::fromStdString(hello.value("name", std::string{})));
    peer.color = QString::fromLatin1(kPalette[(peer.id - 1) % std::size(kPalette)]);
    peer.udpKey = newSessionKey();
    peer.reader.setJsonLimit(kMaxJsonFrame);

    json users = json::array();
    for (const auto& p : peers_) {
        if (p->id != 0) users.push_back(userJson(*p));
    }
    json presence = json::array();
    for (const auto& p : peers_) {
        if (p->id != 0 && p.get() != &peer && !p->presence.empty()) {
            json entry = p->presence;
            entry["id"] = p->id;
            presence.push_back(entry);
        }
    }
    send(peer, {{"t", "welcome"},
                {"v", kProtocolVersion},
                {"id", peer.id},
                {"udpKey", peer.udpKey.toBase64().toStdString()},
                {"color", peer.color.toStdString()},
                {"session", options_.sessionName.toStdString()},
                {"project", options_.projectName.toStdString()},
                {"udp", udp_ != nullptr},
                {"users", users},
                {"presence", presence},
                {"seq", seq_},
                {"scene", SceneSerializer::toJson(*document_)}});
    broadcast({{"t", "userJoined"}, {"user", userJson(peer)}}, &peer);
    log::info("Collab", "{} joined from {}", peer.name.toStdString(), peer.socket->peerAddress().toString().toStdString());
    emit clientsChanged();
}

void CollabServer::applyPatch(Peer& peer, const json& message) {
    const auto body = message.find("patch");
    if (body == message.end()) return;
    auto patch = ScenePatch::fromJson(*body);
    if (!patch) {
        log::warn("Collab", "invalid change from {}: {}", peer.name.toStdString(), patch.error().message);
        return;
    }
    if (auto applied = patch->applyTo(*document_); !applied) {
        log::warn("Collab", "change from {} partly applied: {}", peer.name.toStdString(), applied.error().message);
    }
    ++seq_;
    broadcast({{"t", "patch"}, {"seq", seq_}, {"from", peer.id}, {"patch", patch->toJson()}});

    std::vector<std::string> files;
    for (const json& record : patch->upserts) {
        for (std::string& f : referencedAssetFiles(record)) files.push_back(std::move(f));
    }
    fetchMissingAssets(peer, files);
}

void CollabServer::fetchMissingAssets(Peer& author, const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        const QString path = QString::fromStdString(file);
        if (!isSafeAssetPath(path) || fetching_.contains(path)) continue;
        if (QFileInfo::exists(QDir(options_.projectRoot).filePath(path))) continue;
        fetching_[path] = author.id;
        author.receiver->expect(path);
        send(author, {{"t", "assetGet"}, {"path", file}});
    }
}

void CollabServer::relayPresence(const Peer& from) {
    json message = from.presence;
    message["t"] = "presence";
    message["id"] = from.id;
    const QByteArray frame = encodeMessage(message);
    for (auto& p : peers_) {
        if (p->id == 0 || p.get() == &from) continue;
        if (p->udp && udp_) {
            udp_->writeDatagram(sealDatagram(message, static_cast<quint32>(p->id), p->udpKey), p->udpHost, p->udpPort);
        } else {
            p->socket->write(frame);
        }
    }
}

void CollabServer::onDatagrams() {
    while (udp_ && udp_->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = udp_->receiveDatagram(4096);
        const auto id = datagramParticipant(datagram.data());
        Peer* peer = id ? findPeer(static_cast<int>(*id)) : nullptr;
        if (peer == nullptr) continue;
        const auto message = openDatagram(datagram.data(), peer->udpKey);
        if (!message) continue;  // forged, altered or replayed under another id
        peer->udpHost = datagram.senderAddress();
        peer->udpPort = static_cast<quint16>(datagram.senderPort());
        const std::string type = message->value("t", std::string{});
        if (type == "hello") {
            udp_->writeDatagram(sealDatagram({{"t", "udpAck"}}, static_cast<quint32>(peer->id), peer->udpKey),
                                peer->udpHost, peer->udpPort);
        } else if (type == "presence" && validPresence(*message)) {
            peer->presence = presenceFields(*message);
            relayPresence(*peer);
        }
    }
}

void CollabServer::sendDigest() {
    if (document_ == nullptr || clientCount() == 0) return;
    broadcast({{"t", "digest"}, {"seq", seq_}, {"digest", sceneDigest(SceneSerializer::toJson(*document_)).toStdString()}});
}

} // namespace lumora::collab
