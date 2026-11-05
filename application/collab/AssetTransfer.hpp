#pragma once

#include "collab/Protocol.hpp"

#include <QFile>
#include <QString>

#include <deque>
#include <map>
#include <memory>

class QTcpSocket;

namespace lumora::collab {

/// Streams project files over a socket as chunk frames, keeping at most a couple of chunks
/// queued so large files never balloon memory or starve other messages.
class AssetSender {
public:
    AssetSender(QTcpSocket& socket, QString projectRoot);
    /// False when the path is unsafe, the file is missing or too large.
    bool enqueue(const QString& path);
    /// Writes more chunks while the socket's send buffer is low; call on bytesWritten.
    void pump();
    [[nodiscard]] bool busy() const noexcept { return !queue_.empty(); }

private:
    struct Outgoing {
        QString path;
        std::unique_ptr<QFile> file;
        quint64 total = 0;
        quint64 offset = 0;
    };
    QTcpSocket& socket_;
    QString root_;
    std::deque<Outgoing> queue_;
};

/// Receives chunk streams into "<root>/<path>.part" and renames each file into place once
/// complete. Only expected (requested) paths are accepted, chunks must arrive in order, and an
/// existing file is never overwritten.
class AssetReceiver {
public:
    explicit AssetReceiver(QString projectRoot);
    ~AssetReceiver();

    void expect(const QString& path);
    [[nodiscard]] bool expecting(const QString& path) const { return incoming_.contains(path); }
    void cancel(const QString& path);
    void cancelAll();

    enum class Outcome { Progress, Completed, Rejected };
    Outcome accept(const AssetChunk& chunk, QString* error);

private:
    struct Incoming {
        std::unique_ptr<QFile> file;
        quint64 total = 0;
        quint64 received = 0;
    };
    QString root_;
    std::map<QString, Incoming> incoming_;
};

} // namespace lumora::collab
