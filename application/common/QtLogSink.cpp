#include "common/QtLogSink.hpp"

namespace lumora::app {

void LogBridge::push(const LogMessage& message) {
    UiLogEntry entry;
    entry.level = message.level;
    entry.category = QString::fromStdString(message.category);
    entry.text = QString::fromStdString(message.text);
    entry.time = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(message.time.time_since_epoch()).count());
    {
        std::scoped_lock lock(mutex_);
        history_.push_back(entry);
        if (history_.size() > kHistory) history_.pop_front();
    }
    // Queued automatically when called from a worker thread.
    emit messageLogged(entry);
}

std::vector<UiLogEntry> LogBridge::history() const {
    std::scoped_lock lock(mutex_);
    return {history_.begin(), history_.end()};
}

void LogBridge::clearHistory() {
    std::scoped_lock lock(mutex_);
    history_.clear();
}

} // namespace lumora::app
