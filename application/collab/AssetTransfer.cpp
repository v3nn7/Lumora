#include "collab/AssetTransfer.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTcpSocket>

namespace lumora::collab {

AssetSender::AssetSender(QTcpSocket& socket, QString projectRoot) : socket_(socket), root_(std::move(projectRoot)) {}

bool AssetSender::enqueue(const QString& path) {
    if (!isSafeAssetPath(path)) return false;
    for (const Outgoing& o : queue_) {
        if (o.path == path) return true;  // already on its way
    }
    auto file = std::make_unique<QFile>(QDir(root_).filePath(path));
    const QFileInfo info(*file);
    if (!info.isFile() || static_cast<quint64>(info.size()) > kMaxAssetSize || !file->open(QIODevice::ReadOnly)) {
        return false;
    }
    queue_.push_back({path, std::move(file), static_cast<quint64>(info.size()), 0});
    pump();
    return true;
}

void AssetSender::pump() {
    while (!queue_.empty() && socket_.bytesToWrite() < static_cast<qint64>(2 * kChunkSize)) {
        Outgoing& out = queue_.front();
        AssetChunk chunk;
        chunk.path = out.path;
        chunk.offset = out.offset;
        chunk.total = out.total;
        chunk.data = out.file->read(static_cast<qint64>(kChunkSize));
        if (chunk.data.isEmpty() && out.offset < out.total) {  // file shrank or read error: end it
            chunk.total = out.offset;
        }
        out.offset += static_cast<quint64>(chunk.data.size());
        socket_.write(encodeChunk(chunk));
        if (out.offset >= chunk.total) queue_.pop_front();  // an empty file is a single empty chunk
    }
}

AssetReceiver::AssetReceiver(QString projectRoot) : root_(std::move(projectRoot)) {}

AssetReceiver::~AssetReceiver() { cancelAll(); }

void AssetReceiver::expect(const QString& path) {
    if (isSafeAssetPath(path) && !incoming_.contains(path)) incoming_.emplace(path, Incoming{});
}

void AssetReceiver::cancel(const QString& path) {
    const auto it = incoming_.find(path);
    if (it == incoming_.end()) return;
    if (it->second.file) it->second.file->remove();
    incoming_.erase(it);
}

void AssetReceiver::cancelAll() {
    while (!incoming_.empty()) cancel(incoming_.begin()->first);
}

AssetReceiver::Outcome AssetReceiver::accept(const AssetChunk& chunk, QString* error) {
    const auto it = incoming_.find(chunk.path);
    if (it == incoming_.end()) {
        if (error) *error = QStringLiteral("unexpected file %1").arg(chunk.path);
        return Outcome::Rejected;
    }
    Incoming& in = it->second;
    auto reject = [&](const QString& why) {
        if (error) *error = QStringLiteral("%1: %2").arg(chunk.path, why);
        cancel(chunk.path);
        return Outcome::Rejected;
    };
    if (!in.file) {
        if (chunk.offset != 0 || chunk.total > kMaxAssetSize) return reject(QStringLiteral("invalid size"));
        const QString target = QDir(root_).filePath(chunk.path);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return reject(QStringLiteral("cannot create folder"));
        in.file = std::make_unique<QFile>(target + QStringLiteral(".part"));
        if (!in.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) return reject(in.file->errorString());
        in.total = chunk.total;
    }
    if (chunk.offset != in.received || chunk.total != in.total ||
        in.received + static_cast<quint64>(chunk.data.size()) > in.total) {
        return reject(QStringLiteral("chunk out of order"));
    }
    if (in.file->write(chunk.data) != chunk.data.size()) return reject(in.file->errorString());
    in.received += static_cast<quint64>(chunk.data.size());
    if (in.received < in.total) return Outcome::Progress;

    in.file->close();
    const QString target = QDir(root_).filePath(chunk.path);
    const bool placed = !QFileInfo::exists(target) && in.file->rename(target);
    if (!placed) in.file->remove();  // someone else's copy got there first: keep theirs
    incoming_.erase(it);
    return Outcome::Completed;
}

} // namespace lumora::collab
