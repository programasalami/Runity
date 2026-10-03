#include "waw/core/log.hpp"

#include <chrono>
#include <cstdio>
#include <print>

namespace waw::core {

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

void ConsoleLogSink::write(LogLevel level, std::string_view category, std::string_view message) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    std::FILE* out = level >= LogLevel::Warning ? stderr : stdout;
    std::lock_guard lock(mutex_);
    std::println(out, "{:%H:%M:%S} {:<5} [{}] {}", now, to_string(level), category, message);
    std::fflush(out);
}

void MemoryLogSink::write(LogLevel level, std::string_view category, std::string_view message) {
    std::lock_guard lock(mutex_);
    lines_.push_back({level, std::string(category), std::string(message)});
}

std::vector<MemoryLogSink::Line> MemoryLogSink::lines() const {
    std::lock_guard lock(mutex_);
    return lines_;
}

bool MemoryLogSink::contains(std::string_view text) const {
    std::lock_guard lock(mutex_);
    for (const auto& line : lines_) {
        if (line.message.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace waw::core
