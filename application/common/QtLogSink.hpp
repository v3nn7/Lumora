#pragma once

#include "core/Log.hpp"

#include <QDateTime>
#include <QObject>
#include <QString>

#include <deque>
#include <mutex>
#include <vector>

namespace lumora::app {

struct UiLogEntry {
    LogLevel level = LogLevel::Info;
    QString category;
    QString text;
    QDateTime time;
};

/// Thread-safe bridge from the engine logger to Qt. Keeps a bounded history so panels
/// created later (e.g. the editor console) can show messages logged during startup.
class LogBridge final : public QObject {
    Q_OBJECT
public:
    static constexpr std::size_t kHistory = 5000;

    void push(const LogMessage& message);
    [[nodiscard]] std::vector<UiLogEntry> history() const;
    void clearHistory();

signals:
    void messageLogged(const lumora::app::UiLogEntry& entry);

private:
    mutable std::mutex mutex_;
    std::deque<UiLogEntry> history_;
};

class QtLogSink final : public LogSink {
public:
    explicit QtLogSink(LogBridge& bridge) : bridge_(bridge) {}
    void write(const LogMessage& message) override { bridge_.push(message); }

private:
    LogBridge& bridge_;
};

} // namespace lumora::app

Q_DECLARE_METATYPE(lumora::app::UiLogEntry)
