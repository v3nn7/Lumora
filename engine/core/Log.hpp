#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lumora {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal };

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

struct LogMessage {
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string text;
    std::chrono::system_clock::time_point time;
    std::thread::id thread;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogMessage& message) = 0;
    virtual void flush() {}
};

/// Writes colored output to stderr.
class ConsoleLogSink final : public LogSink {
public:
    void write(const LogMessage& message) override;
    void flush() override;
};

/// Appends plain-text lines to a file. Opens lazily; silently disables itself on I/O failure.
class FileLogSink final : public LogSink {
public:
    explicit FileLogSink(std::string path);
    ~FileLogSink() override;
    void write(const LogMessage& message) override;
    void flush() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Process-wide, thread-safe logger. Sinks are invoked under a lock in registration order.
class Logger {
public:
    static Logger& instance();

    void addSink(std::shared_ptr<LogSink> sink);
    void removeSink(const std::shared_ptr<LogSink>& sink);
    void setMinLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel minLevel() const noexcept;
    [[nodiscard]] bool shouldLog(LogLevel level) const noexcept;

    void log(LogLevel level, std::string_view category, std::string text);
    void flush();

private:
    Logger();
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    LogLevel minLevel_;
};

[[nodiscard]] std::string formatLogLine(const LogMessage& message);

namespace log {

template <class... Args>
void write(LogLevel level, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    Logger& logger = Logger::instance();
    if (!logger.shouldLog(level)) {
        return;
    }
    logger.log(level, category, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void trace(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) { write(LogLevel::Trace, cat, fmt, std::forward<Args>(args)...); }
template <class... Args>
void debug(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) { write(LogLevel::Debug, cat, fmt, std::forward<Args>(args)...); }
template <class... Args>
void info(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) { write(LogLevel::Info, cat, fmt, std::forward<Args>(args)...); }
template <class... Args>
void warn(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) { write(LogLevel::Warn, cat, fmt, std::forward<Args>(args)...); }
template <class... Args>
void error(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) { write(LogLevel::Error, cat, fmt, std::forward<Args>(args)...); }
template <class... Args>
void fatal(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) { write(LogLevel::Fatal, cat, fmt, std::forward<Args>(args)...); }

} // namespace log
} // namespace lumora
