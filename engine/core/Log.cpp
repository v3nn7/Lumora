#include "core/Log.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace lumora {

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Fatal: return "fatal";
    }
    return "unknown";
}

std::string formatLogLine(const LogMessage& message) {
    const auto timeT = std::chrono::system_clock::to_time_t(message.time);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(message.time.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(LUMORA_PLATFORM_WINDOWS)
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif
    return std::format("[{:02}:{:02}:{:02}.{:03}] [{}] [{}] {}", tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                       toString(message.level), message.category, message.text);
}

void ConsoleLogSink::write(const LogMessage& message) {
    const char* color = "";
    switch (message.level) {
        case LogLevel::Trace: color = "\x1b[90m"; break;
        case LogLevel::Debug: color = "\x1b[36m"; break;
        case LogLevel::Info:  color = "\x1b[0m"; break;
        case LogLevel::Warn:  color = "\x1b[33m"; break;
        case LogLevel::Error: color = "\x1b[31m"; break;
        case LogLevel::Fatal: color = "\x1b[1;41m"; break;
    }
    const std::string line = formatLogLine(message);
    std::fprintf(stderr, "%s%s\x1b[0m\n", color, line.c_str());
}

void ConsoleLogSink::flush() { std::fflush(stderr); }

struct FileLogSink::Impl {
    std::string path;
    std::ofstream stream;
    bool failed = false;
};

FileLogSink::FileLogSink(std::string path) : impl_(std::make_unique<Impl>()) { impl_->path = std::move(path); }

FileLogSink::~FileLogSink() = default;

void FileLogSink::write(const LogMessage& message) {
    if (impl_->failed) {
        return;
    }
    if (!impl_->stream.is_open()) {
        impl_->stream.open(impl_->path, std::ios::out | std::ios::app);
        if (!impl_->stream) {
            impl_->failed = true;
            return;
        }
    }
    impl_->stream << formatLogLine(message) << '\n';
    if (message.level >= LogLevel::Error) {
        impl_->stream.flush();
    }
}

void FileLogSink::flush() {
    if (impl_->stream.is_open()) {
        impl_->stream.flush();
    }
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger()
#if defined(LUMORA_DEBUG)
    : minLevel_(LogLevel::Debug)
#else
    : minLevel_(LogLevel::Info)
#endif
{
}

void Logger::addSink(std::shared_ptr<LogSink> sink) {
    std::scoped_lock lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::removeSink(const std::shared_ptr<LogSink>& sink) {
    std::scoped_lock lock(mutex_);
    std::erase(sinks_, sink);
}

void Logger::setMinLevel(LogLevel level) noexcept {
    std::scoped_lock lock(mutex_);
    minLevel_ = level;
}

LogLevel Logger::minLevel() const noexcept {
    std::scoped_lock lock(mutex_);
    return minLevel_;
}

bool Logger::shouldLog(LogLevel level) const noexcept { return level >= minLevel(); }

void Logger::log(LogLevel level, std::string_view category, std::string text) {
    LogMessage message{level, std::string(category), std::move(text), std::chrono::system_clock::now(),
                       std::this_thread::get_id()};
    std::scoped_lock lock(mutex_);
    for (const auto& sink : sinks_) {
        sink->write(message);
    }
}

void Logger::flush() {
    std::scoped_lock lock(mutex_);
    for (const auto& sink : sinks_) {
        sink->flush();
    }
}

} // namespace lumora
