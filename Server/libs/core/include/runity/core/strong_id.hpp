#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace runity::core {

/// A typed integer id: an EntityId cannot be passed where a SessionId is expected.
template <class Tag, class Rep = std::uint32_t>
class StrongId {
public:
    using rep_type = Rep;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;

private:
    Rep value_{};
};

}  // namespace runity::core

template <class Tag, class Rep>
struct std::hash<runity::core::StrongId<Tag, Rep>> {
    std::size_t operator()(runity::core::StrongId<Tag, Rep> id) const noexcept { return std::hash<Rep>{}(id.value()); }
};
