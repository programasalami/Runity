#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace runity::core {

/// The value of an environment variable, or nullopt if it is not set.
[[nodiscard]] inline std::optional<std::string> env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

}  // namespace runity::core
