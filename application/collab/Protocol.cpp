#include "collab/Protocol.hpp"

#include "scene/ComponentRegistry.hpp"

extern "C" {
#include <monocypher.h>
}

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtEndian>
#include <QUrl>

#include <algorithm>
#include <cstring>

namespace lumora::collab {

using nlohmann::json;

namespace {
constexpr qsizetype kHeaderSize = 8;  // magic + participant id (authenticated, not encrypted)
}

namespace {

QByteArray frame(FrameKind kind, const QByteArray& payload) {
    QByteArray out;
    out.resize(5);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size() + 1), out.data());
    out[4] = static_cast<char>(kind);
    out.append(payload);
    return out;
}

} // namespace

QByteArray encodeMessage(const json& message) {
    const std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    return frame(FrameKind::Json, QByteArray(text.data(), static_cast<qsizetype>(text.size())));
}

QByteArray encodeChunk(const AssetChunk& chunk) {
    const QByteArray path = chunk.path.toUtf8();
    QByteArray payload;
    payload.resize(2 + path.size() + 16);
    char* p = payload.data();
    qToBigEndian<quint16>(static_cast<quint16>(path.size()), p);
    std::memcpy(p + 2, path.constData(), static_cast<std::size_t>(path.size()));
    qToBigEndian<quint64>(chunk.offset, p + 2 + path.size());
    qToBigEndian<quint64>(chunk.total, p + 2 + path.size() + 8);
    payload.append(chunk.data);
    return frame(FrameKind::AssetChunk, payload);
}

QByteArray newSessionKey() {
    QByteArray key(kSessionKeySize, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(key.data()), kSessionKeySize / 4);
    return key;
}

QByteArray sealDatagram(const json& message, quint32 participant, const QByteArray& key) {
    if (key.size() != kSessionKeySize) return {};
    const std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    QByteArray out(kHeaderSize + 24 + 16 + static_cast<qsizetype>(text.size()), '\0');
    std::memcpy(out.data(), kUdpMagic, 4);
    qToBigEndian<quint32>(participant, out.data() + 4);
    auto* nonce = reinterpret_cast<uint8_t*>(out.data() + kHeaderSize);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(nonce), 6);
    crypto_aead_lock(reinterpret_cast<uint8_t*>(out.data() + kHeaderSize + 40), nonce + 24,
                     reinterpret_cast<const uint8_t*>(key.constData()), nonce,
                     reinterpret_cast<const uint8_t*>(out.constData()), kHeaderSize,
                     reinterpret_cast<const uint8_t*>(text.data()), text.size());
    return out;
}

std::optional<quint32> datagramParticipant(const QByteArray& datagram) {
    if (datagram.size() < kHeaderSize + 40 + 2 || datagram.size() > 4096 ||
        std::memcmp(datagram.constData(), kUdpMagic, 4) != 0) {
        return std::nullopt;
    }
    return qFromBigEndian<quint32>(datagram.constData() + 4);
}

std::optional<json> openDatagram(const QByteArray& datagram, const QByteArray& key) {
    if (!datagramParticipant(datagram) || key.size() != kSessionKeySize) return std::nullopt;
    const auto* bytes = reinterpret_cast<const uint8_t*>(datagram.constData());
    QByteArray plain(datagram.size() - kHeaderSize - 40, '\0');
    if (crypto_aead_unlock(reinterpret_cast<uint8_t*>(plain.data()), bytes + kHeaderSize + 24,
                           reinterpret_cast<const uint8_t*>(key.constData()), bytes + kHeaderSize, bytes, kHeaderSize,
                           bytes + kHeaderSize + 40, static_cast<size_t>(plain.size())) != 0) {
        return std::nullopt;  // forged or altered
    }
    if (!jsonNestingWithin(plain, 16)) return std::nullopt;
    json message = json::parse(plain.constData(), plain.constData() + plain.size(), nullptr, false);
    if (message.is_discarded() || !message.is_object()) return std::nullopt;
    return message;
}

void FrameReader::append(const QByteArray& bytes) {
    if (!failed()) buffer_.append(bytes);
}

std::optional<Frame> FrameReader::next() {
    if (failed() || buffer_.size() < 5) return std::nullopt;
    const quint32 length = qFromBigEndian<quint32>(buffer_.constData());
    const auto kind = static_cast<FrameKind>(static_cast<std::uint8_t>(buffer_[4]));
    const std::uint32_t limit = kind == FrameKind::AssetChunk ? kChunkSize + 1024 : jsonLimit_;
    if (length == 0 || length > limit) {
        error_ = QStringLiteral("frame of %1 bytes exceeds the limit").arg(length);
        return std::nullopt;
    }
    if (buffer_.size() < static_cast<qsizetype>(4 + length)) return std::nullopt;
    const QByteArray payload = buffer_.mid(5, static_cast<qsizetype>(length) - 1);
    buffer_.remove(0, static_cast<qsizetype>(4 + length));

    Frame frame;
    frame.kind = kind;
    switch (kind) {
        case FrameKind::Json: {
            if (!jsonNestingWithin(payload, 64)) {
                error_ = QStringLiteral("message nested too deeply");
                return std::nullopt;
            }
            frame.message = json::parse(payload.constData(), payload.constData() + payload.size(), nullptr, false);
            if (frame.message.is_discarded() || !frame.message.is_object() || !frame.message.contains("t") ||
                !frame.message["t"].is_string()) {
                error_ = QStringLiteral("malformed message");
                return std::nullopt;
            }
            return frame;
        }
        case FrameKind::AssetChunk: {
            if (payload.size() < 2) break;
            const quint16 pathLength = qFromBigEndian<quint16>(payload.constData());
            if (payload.size() < 2 + pathLength + 16) break;
            frame.chunk.path = QString::fromUtf8(payload.constData() + 2, pathLength);
            frame.chunk.offset = qFromBigEndian<quint64>(payload.constData() + 2 + pathLength);
            frame.chunk.total = qFromBigEndian<quint64>(payload.constData() + 2 + pathLength + 8);
            frame.chunk.data = payload.mid(2 + pathLength + 16);
            return frame;
        }
    }
    error_ = QStringLiteral("malformed frame");
    return std::nullopt;
}

bool jsonNestingWithin(const QByteArray& text, int limit) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > limit) return false;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return true;
}

bool isSafeAssetPath(const QString& path) {
    if (path.isEmpty() || path.size() > 512 || !path.startsWith(QLatin1String("Content/"))) return false;
    if (path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':')) || path.contains(QChar(0))) return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    return std::ranges::none_of(parts, [](const QString& part) {
        return part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..") ||
               part.endsWith(QLatin1String(".part"));
    });
}

std::vector<std::string> referencedAssetFiles(const json& record) {
    std::vector<std::string> files;
    const auto components = record.find("components");
    if (components == record.end() || !components->is_object()) return files;
    for (const auto& [componentId, fields] : components->items()) {
        const ComponentDesc* desc = ComponentRegistry::builtin().find(componentId);
        if (desc == nullptr || !fields.is_object()) continue;
        for (const FieldDesc& field : desc->fields) {
            if (field.kind != FieldKind::Asset) continue;
            const auto value = fields.find(field.id);
            if (value == fields.end() || !value->is_string()) continue;
            std::string key = value->get<std::string>();
            if (const auto hash = key.find('#'); hash != std::string::npos) key.resize(hash);
            if (!key.empty() && std::ranges::find(files, key) == files.end()) files.push_back(std::move(key));
        }
    }
    return files;
}

std::vector<std::string> referencedAssetFilesOfScene(const json& document) {
    std::vector<std::string> files;
    const auto entities = document.find("entities");
    if (entities == document.end() || !entities->is_array()) return files;
    for (const json& record : *entities) {
        for (std::string& file : referencedAssetFiles(record)) {
            if (std::ranges::find(files, file) == files.end()) files.push_back(std::move(file));
        }
    }
    return files;
}

std::vector<std::string> gltfDependencies(const QByteArray& gltfJson) {
    std::vector<std::string> files;
    const json doc = json::parse(gltfJson.constData(), gltfJson.constData() + gltfJson.size(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return files;
    for (const char* list : {"buffers", "images"}) {
        const auto it = doc.find(list);
        if (it == doc.end() || !it->is_array()) continue;
        for (const json& item : *it) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) continue;
            const std::string uri = item["uri"].get<std::string>();
            if (uri.starts_with("data:")) continue;
            const QString decoded = QUrl::fromPercentEncoding(QByteArray::fromStdString(uri));
            files.push_back(decoded.toStdString());
        }
    }
    return files;
}

std::optional<Address> parseAddress(const QString& input) {
    QString text = input.trimmed();
    for (const QString& scheme : {QStringLiteral("lumora://"), QStringLiteral("tcp://")}) {
        if (text.startsWith(scheme, Qt::CaseInsensitive)) text = text.mid(scheme.size());
    }
    Address address;
    if (const qsizetype hash = text.indexOf(QLatin1Char('#')); hash >= 0) {
        const QByteArray fingerprint = text.mid(hash + 1).toLatin1().toLower().replace(":", "").replace(" ", "");
        if (fingerprint.size() != 64 || QByteArray::fromHex(fingerprint).toHex() != fingerprint) return std::nullopt;
        address.fingerprint = fingerprint;
        text = text.left(hash);
    }
    while (text.endsWith(QLatin1Char('/'))) text.chop(1);
    if (text.isEmpty() || text.contains(QLatin1Char(' '))) return std::nullopt;
    QString portText;
    if (text.startsWith(QLatin1Char('['))) {  // [ipv6]:port
        const qsizetype close = text.indexOf(QLatin1Char(']'));
        if (close < 0) return std::nullopt;
        address.host = text.mid(1, close - 1);
        const QString rest = text.mid(close + 1);
        if (!rest.isEmpty()) {
            if (!rest.startsWith(QLatin1Char(':'))) return std::nullopt;
            portText = rest.mid(1);
        }
    } else if (text.count(QLatin1Char(':')) == 1) {
        address.host = text.section(QLatin1Char(':'), 0, 0);
        portText = text.section(QLatin1Char(':'), 1);
    } else {
        address.host = text;  // plain host, or a bare IPv6 address
    }
    if (address.host.isEmpty()) return std::nullopt;
    if (!portText.isEmpty()) {
        bool ok = false;
        const uint port = portText.toUInt(&ok);
        if (!ok || port == 0 || port > 65535) return std::nullopt;
        address.port = static_cast<quint16>(port);
    }
    return address;
}

QString formatAddress(const Address& address) {
    const QString host = address.host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(address.host) : address.host;
    QString text = QStringLiteral("%1:%2").arg(host).arg(address.port);
    if (!address.fingerprint.isEmpty()) text += QLatin1Char('#') + QString::fromLatin1(address.fingerprint);
    return text;
}

QString shareAddress(const QString& host, quint16 port, const QByteArray& fingerprint) {
    return QStringLiteral("lumora://") + formatAddress({host, port, fingerprint});
}

QString sanitizeName(const QString& name) {
    QString out;
    for (const QChar c : name.trimmed()) {
        if (c.isPrint()) out.append(c);
        if (out.size() >= kMaxNameLength) break;
    }
    return out.isEmpty() ? QStringLiteral("Guest") : out;
}

QString sceneDigest(const json& document) {
    const std::string text = document.dump();
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())),
                                 QCryptographicHash::Sha1)
            .toHex()
            .left(20));
}

} // namespace lumora::collab
