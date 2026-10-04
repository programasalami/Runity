#include "runity/sim/projectile_path.hpp"

#include <cmath>
#include <numbers>

namespace runity::sim {

Vec2 path_offset(const PathSpec& spec, float elapsed_ms, std::uint32_t bullet_id, float angle) noexcept {
    constexpr float pi = std::numbers::pi_v<float>;
    const float phase = bullet_id % 2 == 0 ? 0.0f : pi;
    float t = elapsed_ms;
    switch (spec.kind) {
        case content::PathKind::Boomerang: {
            // Out for half the lifetime, then back the same way (reference BoomerangPath).
            const float half = static_cast<float>(spec.lifetime_ms) / 2.0f;
            if (t > half) t = static_cast<float>(spec.lifetime_ms) - t;
            const float d = t * (spec.speed / 1000.0f);
            return {d * std::cos(angle), d * std::sin(angle)};
        }
        case content::PathKind::Wavy: {
            const float theta = angle + pi / 64.0f * std::sin(phase + 6.0f * pi * t / 1000.0f);
            const float d = t * (spec.speed / 1000.0f);
            return {d * std::cos(theta), d * std::sin(theta)};
        }
        case content::PathKind::Amplitude: {
            const float d = t * (spec.speed / 1000.0f);
            Vec2 p{d * std::cos(angle), d * std::sin(angle)};
            const float life = spec.lifetime_ms > 0 ? static_cast<float>(spec.lifetime_ms) : 1.0f;
            const float deflection = spec.amplitude * std::sin(phase + t / life * spec.frequency * 2.0f * pi);
            p.x += deflection * std::cos(angle + pi / 2.0f);
            p.y += deflection * std::sin(angle + pi / 2.0f);
            return p;
        }
        case content::PathKind::Line:
        default: {
            const float d = t * (spec.speed / 1000.0f);
            return {d * std::cos(angle), d * std::sin(angle)};
        }
    }
}

}  // namespace runity::sim
