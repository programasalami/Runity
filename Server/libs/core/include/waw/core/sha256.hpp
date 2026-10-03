#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace waw::core {

/// SHA-256 (FIPS 180-4). Used to turn join tickets into Redis keys exactly as the Account/API service does
/// (lower-case hex of the ticket's UTF-8 bytes). Not used for passwords - the game server never sees one.
class Sha256 {
public:
    Sha256() noexcept;
    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view text) noexcept;
    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

    [[nodiscard]] static std::string hex(std::string_view text);

private:
    void block(const std::uint8_t* p) noexcept;

    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

}  // namespace waw::core
