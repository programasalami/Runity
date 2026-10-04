#pragma once

#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace runity::core {

enum class LogLevel { Debug, Info, Warning, Error };

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

/// Where log lines go. Implementations must be thread-safe.
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(LogLevel level, std::string_view category, std::string_view message) = 0;
};

/// Writes "time level [category] message" lines to stdout (Debug/Info) or stderr (Warning/Error).
class ConsoleLogSink final : public ILogSink {
public:
    void write(LogLevel level, std::string_view category, std::string_view message) override;

private:
    std::mutex mutex_;
};

/// Keeps lines in memory (tests).
class MemoryLogSink final : public ILogSink {
public:
    struct Line {
        LogLevel level;
        std::string category;
        std::string message;
    };
    void write(LogLevel level, std::string_view category, std::string_view message) override;
    [[nodiscard]] std::vector<Line> lines() const;
    [[nodiscard]] bool contains(std::string_view text) const;

private:
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
};

/// A named logger bound to a sink. Cheap to copy; the sink must outlive it.
class Logger {
public:
    Logger(ILogSink& sink, std::string category, LogLevel min_level = LogLevel::Info)
        : sink_(&sink), category_(std::move(category)), min_level_(min_level) {}

    [[nodiscard]] Logger child(std::string_view category) const { return Logger(*sink_, std::string(category), min_level_); }

    template <class... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Warning, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    [[nodiscard]] bool enabled(LogLevel level) const noexcept { return level >= min_level_; }

private:
    template <class... Args>
    void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) const {
        if (!enabled(level)) return;
        sink_->write(level, category_, std::format(fmt, std::forward<Args>(args)...));
    }

    ILogSink* sink_;
    std::string category_;
    LogLevel min_level_;
};

}  // namespace runity::core
