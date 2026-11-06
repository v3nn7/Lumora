#include "collab/CollabClient.hpp"
#include <QPointer>

#include "collab/AssetTransfer.hpp"
#include "core/Log.hpp"
#include "scene/ScenePatch.hpp"

#include <QNetworkDatagram>
#include "collab/HostIdentity.hpp"

#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUdpSocket>

#include <algorithm>

namespace lumora::collab {

namespace {
constexpr int kConnectTimeoutMs = 15000;  // TCP connect + TLS handshake
}

using nlohmann::json;

namespace {

constexpr int kHeartbeatMs = 1000;
constexpr qint64 kUdpTimeoutMs = 5000;

json vec3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 vec3From(const json& j, glm::vec3 fallback) {
    if (!j.is_array() || j.size() != 3) return fallback;
    for (const json& c : j) {
        if (!c.is_number()) return fallback;
    }
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

Participant participantFrom(const json& u) {
    Participant p;
    p.id = u.value("id", 0);
    p.name = sanitizeName(QString::fromStdString(u.value("name", std::string{})));
    p.color = QColor(QString::fromStdString(u.value("color", std::string("#ffffff"))));
    if (!p.color.isValid()) p.color = Qt::white;
    return p;
}

} // namespace

CollabClient::CollabClient(QObject* parent) : QObject(parent) {
    clock_.start();
    heartbeat_.setInterval(kHeartbeatMs);
    connect(&heartbeat_, &QTimer::timeout, this, &CollabClient::heartbeat);
}

CollabClient::~CollabClient() {
    if (socket_) socket_->disconnect(this);
}

void CollabClient::setProjectRoot(const QString& root) {
    root_ = root;
    if (socket_) {
        sender_ = std::make_unique<AssetSender>(*socket_, root_);
        receiver_ = std::make_unique<AssetReceiver>(root_);
    }
}

void CollabClient::connectTo(const Address& address, const QString& name, const QString& password) {
    disconnectFromHost();
    address_ = address;
    name_ = sanitizeName(name);
    password_ = password;
    state_ = State::Connecting;
    reader_ = FrameReader{};
    socket_ = std::make_unique<QSslSocket>();
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    QSslConfiguration tls = socket_->sslConfiguration();
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    socket_->setSslConfiguration(tls);
    hostFingerprint_.clear();
    sender_ = std::make_unique<AssetSender>(*socket_, root_);
    receiver_ = std::make_unique<AssetReceiver>(root_);
    connect(socket_.get(), &QSslSocket::sslErrors, this, &CollabClient::onSslErrors);
    connect(socket_.get(), &QSslSocket::encrypted, this, [this] {
        hostFingerprint_ = certificateFingerprint(socket_->peerCertificate());
        log::info("Collab", "secure connection ({}), host certificate {}", encryption().toStdString(),
                  formatFingerprint(hostFingerprint_, 4).toStdString());
        send({{"t", "hello"},
              {"v", kProtocolVersion},
              {"name", name_.toStdString()},
              {"password", password_.toStdString()}});
    });
    connect(socket_.get(), &QSslSocket::readyRead, this, &CollabClient::onReadable);
    connect(socket_.get(), &QSslSocket::bytesWritten, this, [this] {
        if (sender_) sender_->pump();
    });
    connect(socket_.get(), &QSslSocket::disconnected, this, [this] { finish(tr("The host closed the session.")); });
    connect(socket_.get(), &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (socket_) finish(socket_->errorString());
    });
    if (!QSslSocket::supportsSsl()) {
        QTimer::singleShot(0, this, [this] { finish(tr("TLS is not available on this computer (OpenSSL is missing).")); });
        return;
    }
    log::info("Collab", "connecting to {}", formatAddress(address).toStdString());
    socket_->connectToHostEncrypted(address.host, address.port);
    // A tunnel with nothing behind it accepts the connection and then stays silent: give up
    // with a reason instead of waiting for the operating system's (minutes long) timeout.
    QTimer::singleShot(kConnectTimeoutMs, this, [this, socket = QPointer<QSslSocket>(socket_.get())] {
        if (socket == nullptr || socket != socket_.get() || state_ != State::Connecting || socket_->isEncrypted()) return;
        finish(tr("No answer from %1 within %2 seconds. Check that the host is still in the session and that the "
                  "address is right. With a tunnel (playit.gg), check that it is running and forwards TCP to the "
                  "port the host chose (7777 unless changed).")
                   .arg(formatAddress({address_.host, address_.port, {}}))
                   .arg(kConnectTimeoutMs / 1000));
    });
}

void CollabClient::onSslErrors(const QList<QSslError>&) {
    // A session host's certificate is self-signed: trust comes from its fingerprint instead.
    const QByteArray fingerprint = certificateFingerprint(socket_->peerCertificate());
    const QByteArray expected = !address_.fingerprint.isEmpty()
                                    ? address_.fingerprint
                                    : KnownHosts::fingerprintFor(address_.host, address_.port);
    if (!expected.isEmpty() && fingerprint == expected) {
        socket_->ignoreSslErrors();
        return;
    }
    if (!expected.isEmpty()) {
        log::error("Collab", "host certificate {} does not match the expected {}",
                   formatFingerprint(fingerprint, 4).toStdString(), formatFingerprint(expected, 4).toStdString());
        return finish(tr("The host's certificate does not match the one expected for this address. "
                         "Someone may be intercepting the connection - not connecting."));
    }
    const QString host = address_.host;
    const quint16 port = address_.port;
    emit untrustedHost(host, port, fingerprint);
    finish(tr("The host's identity has not been confirmed yet."));
}

QString CollabClient::encryption() const {
    if (!socket_ || !socket_->isEncrypted()) return tr("not encrypted");
    const QSslCipher cipher = socket_->sessionCipher();
    return QStringLiteral("%1, %2").arg(cipher.protocolString(), cipher.name());
}

void CollabClient::disconnectFromHost() {
    if (!socket_) return;
    finish(tr("You left the session."));
}

void CollabClient::finish(const QString& reason) {
    if (!socket_) return;
    const State was = state_;
    std::unique_ptr<QTcpSocket> socket = std::move(socket_);
    socket->disconnect(this);
    socket->abort();
    socket.release()->deleteLater();  // we may be inside one of its signals
    sender_.reset();
    if (receiver_) receiver_->cancelAll();
    receiver_.reset();
    udp_.reset();
    heartbeat_.stop();
    state_ = State::Disconnected;
    id_ = 0;
    participants_.clear();
    latency_.reset();
    udpReported_ = false;
    if (was == State::Joined) {
        log::info("Collab", "left the session: {}", reason.toStdString());
        emit participantsChanged();
        emit left(reason);
    } else if (was == State::Connecting) {
        log::warn("Collab", "could not join {}: {}", formatAddress({address_.host, address_.port, {}}).toStdString(),
                  reason.toStdString());
        emit joinFailed(reason);
    }
}

bool CollabClient::udpActive() const noexcept {
    return udp_ != nullptr && sinceUdpAck_.isValid() && sinceUdpAck_.elapsed() < kUdpTimeoutMs;
}

void CollabClient::send(const json& message) {
    if (socket_ && socket_->state() == QAbstractSocket::ConnectedState) socket_->write(encodeMessage(message));
}

void CollabClient::sendDatagram(const json& message) {
    if (!udp_ || udpTarget_.isNull()) return;
    json m = message;
    udp_->writeDatagram(sealDatagram(m, static_cast<quint32>(id_), udpKey_), udpTarget_, address_.port);
}

void CollabClient::sendPatch(const ScenePatch& patch) {
    if (state_ == State::Joined) send({{"t", "patch"}, {"patch", patch.toJson()}});
}

void CollabClient::sendPresence(const glm::vec3& position, const glm::vec3& forward, const QString& selection) {
    if (state_ != State::Joined) return;
    json m = {{"t", "presence"}, {"p", vec3(position)}, {"f", vec3(forward)}, {"s", selection.toStdString()}};
    if (udpActive()) {
        sendDatagram(m);
    } else {
        send(m);
    }
}

void CollabClient::sendChat(const QString& text) {
    const QString trimmed = text.trimmed().left(kMaxChatLength);
    if (state_ == State::Joined && !trimmed.isEmpty()) send({{"t", "chat"}, {"text", trimmed.toStdString()}});
}

void CollabClient::requestAsset(const QString& path) {
    if (state_ != State::Joined || !receiver_ || !isSafeAssetPath(path) || receiver_->expecting(path)) return;
    receiver_->expect(path);
    send({{"t", "assetGet"}, {"path", path.toStdString()}});
}

void CollabClient::requestResync() { send({{"t", "resync"}}); }

void CollabClient::onReadable() {
    if (!socket_) return;
    reader_.append(socket_->readAll());
    while (auto frame = reader_.next()) {
        handle(*frame);
        if (!socket_) return;  // the session ended inside the handler
    }
    if (reader_.failed()) finish(tr("Protocol error: %1").arg(reader_.error()));
}

void CollabClient::handle(const Frame& frame) {
    if (frame.kind == FrameKind::AssetChunk) {
        if (!receiver_) return;
        QString error;
        switch (receiver_->accept(frame.chunk, &error)) {
            case AssetReceiver::Outcome::Progress:
                break;
            case AssetReceiver::Outcome::Completed:
                log::info("Collab", "downloaded {}", frame.chunk.path.toStdString());
                emit assetReceived(frame.chunk.path);
                break;
            case AssetReceiver::Outcome::Rejected:
                log::warn("Collab", "download failed: {}", error.toStdString());
                break;
        }
        return;
    }
    const json& m = frame.message;
    const std::string type = m["t"].get<std::string>();
    if (state_ == State::Connecting) {
        if (type == "welcome") return onWelcome(m);
        if (type == "reject") return finish(QString::fromStdString(m.value("reason", std::string("Refused."))));
        return;
    }
    if (type == "patch") {
        auto patch = ScenePatch::fromJson(m.value("patch", json::object()));
        if (!patch) {
            log::warn("Collab", "invalid change from the host: {}", patch.error().message);
            return;
        }
        emit patchReceived(*patch, m.value("from", 0), m.value("seq", quint64{0}));
    } else if (type == "snapshot") {
        emit snapshotReceived(m.value("scene", json::object()), m.value("seq", quint64{0}));
    } else if (type == "digest") {
        emit digestReceived(m.value("seq", quint64{0}), QString::fromStdString(m.value("digest", std::string{})));
    } else if (type == "presence") {
        updatePresence(m);
    } else if (type == "userJoined") {
        const Participant p = participantFrom(m.value("user", json::object()));
        if (p.id != 0) {
            participants_[p.id] = p;
            emit participantsChanged();
        }
    } else if (type == "userLeft") {
        participants_.erase(m.value("id", 0));
        emit participantsChanged();
    } else if (type == "chat") {
        emit chatReceived(m.value("from", 0), QString::fromStdString(m.value("text", std::string{})).left(kMaxChatLength));
    } else if (type == "assetGet") {  // the host fetches a file our change refers to
        const QString path = QString::fromStdString(m.value("path", std::string{}));
        if (!sender_ || !sender_->enqueue(path)) send({{"t", "assetMissing"}, {"path", path.toStdString()}});
    } else if (type == "assetMissing") {
        const QString path = QString::fromStdString(m.value("path", std::string{}));
        if (receiver_) receiver_->cancel(path);
        emit assetUnavailable(path);
    } else if (type == "assetAvailable") {
        emit assetAvailable(QString::fromStdString(m.value("path", std::string{})));
    } else if (type == "pong") {
        latency_ = static_cast<int>(clock_.elapsed() - static_cast<qint64>(m.value("time", 0.0)));
        emit connectionInfoChanged();
    }
}

void CollabClient::onWelcome(const json& m) {
    id_ = m.value("id", 0);
    udpKey_ = QByteArray::fromBase64(QByteArray::fromStdString(m.value("udpKey", std::string{})));
    session_ = QString::fromStdString(m.value("session", std::string{}));
    project_ = QString::fromStdString(m.value("project", std::string{}));
    hostUdp_ = m.value("udp", false) && udpKey_.size() == kSessionKeySize;
    participants_.clear();
    if (const auto users = m.find("users"); users != m.end() && users->is_array()) {
        for (const json& u : *users) {
            const Participant p = participantFrom(u);
            if (p.id != 0) participants_[p.id] = p;
        }
    }
    if (const auto presence = m.find("presence"); presence != m.end() && presence->is_array()) {
        for (const json& entry : *presence) updatePresence(entry);
    }
    state_ = State::Joined;
    if (hostUdp_) {
        udp_ = std::make_unique<QUdpSocket>();
        udpTarget_ = socket_->peerAddress();  // already resolved (works for tunnel host names)
        if (udp_->bind(udpTarget_.protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress::AnyIPv6
                                                                                : QHostAddress::AnyIPv4,
                       0)) {
            connect(udp_.get(), &QUdpSocket::readyRead, this, &CollabClient::onDatagrams);
        } else {
            udp_.reset();
        }
    }
    heartbeat_.start();
    heartbeat();
    log::info("Collab", "joined '{}' as {} ({} participants)", session_.toStdString(), name_.toStdString(),
              participants_.size());
    emit joined(m.value("scene", json::object()), m.value("seq", quint64{0}));
    emit participantsChanged();
}

void CollabClient::updatePresence(const json& m) {
    const auto it = participants_.find(m.value("id", 0));
    if (it == participants_.end() || it->first == id_) return;
    Participant& p = it->second;
    if (m.contains("p")) {
        p.position = vec3From(m["p"], p.position);
        p.hasPose = true;
    }
    if (m.contains("f")) p.forward = vec3From(m["f"], p.forward);
    if (m.contains("s") && m["s"].is_string()) p.selection = QString::fromStdString(m["s"].get<std::string>());
    emit presenceChanged(p.id);
}

void CollabClient::onDatagrams() {
    while (udp_ && udp_->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = udp_->receiveDatagram(4096);
        const auto message = openDatagram(datagram.data(), udpKey_);
        if (!message) continue;  // not from the host (or altered)
        const bool wasActive = udpActive();
        sinceUdpAck_.restart();
        if (!wasActive) heartbeat();  // report the switch to UDP right away
        if (message->value("t", std::string{}) == "presence") updatePresence(*message);
    }
}

void CollabClient::heartbeat() {
    if (state_ != State::Joined) return;
    send({{"t", "ping"}, {"time", static_cast<double>(clock_.elapsed())}});
    if (udp_) sendDatagram({{"t", "hello"}});
    const bool active = udpActive();
    if (active != udpReported_) {
        udpReported_ = active;
        send({{"t", "presenceMode"}, {"udp", active}});
        log::info("Collab", "presence via {}", active ? "UDP" : "TCP");
        emit connectionInfoChanged();
    }
}

} // namespace lumora::collab
