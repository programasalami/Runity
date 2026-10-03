#include "waw/sim/rules.hpp"

#include <algorithm>
#include <cmath>

namespace waw::sim::rules {

BaseStats starting_stats(const content::PlayerClassDesc& cls) noexcept {
    BaseStats out{};
    for (std::size_t i = 0; i < content::kStatCount; ++i) out[i] = cls.stats[i].start;
    return out;
}

void level_up(BaseStats& stats, const content::PlayerClassDesc& cls, std::mt19937_64& rng) {
    for (std::size_t i = 0; i < content::kStatCount; ++i) {
        const auto& inc = cls.level_increase[i];
        if (inc.max <= 0) continue;
        std::uniform_int_distribution<int> roll(std::min(inc.min, inc.max), inc.max);
        stats[i] = std::min(stats[i] + roll(rng), cls.stats[i].max);
    }
}

PlayerStats with_gear(const BaseStats& base, const std::vector<int>& items, const content::ContentDb& content) noexcept {
    using content::Stat;
    auto total = base;
    for (int i = 0; i < kGearSlots && i < static_cast<int>(items.size()); ++i) {
        if (items[static_cast<std::size_t>(i)] < 0) continue;
        const auto* item = content.item(static_cast<std::uint16_t>(items[static_cast<std::size_t>(i)]));
        if (item == nullptr) continue;
        for (std::size_t s = 0; s < content::kStatCount; ++s) total[s] += item->stat_bonuses[s];
    }
    auto s = [&](Stat stat) { return total[static_cast<std::size_t>(stat)]; };
    return PlayerStats{s(Stat::MaxHp), s(Stat::MaxMp), s(Stat::Attack), s(Stat::Defense),
                       s(Stat::Speed), s(Stat::Dexterity), s(Stat::HpRegen), s(Stat::MpRegen)};
}

bool slot_fits(int slot, const std::vector<int>& class_slot_types, int item_slot_type) noexcept {
    if (slot < 0 || slot >= kPlayerSlots) return false;
    if (slot >= kGearSlots) return true;
    return slot < static_cast<int>(class_slot_types.size()) && class_slot_types[static_cast<std::size_t>(slot)] == item_slot_type;
}

std::int64_t xp_to_next_level(int level) noexcept {
    const float l = static_cast<float>(level);
    return static_cast<std::int64_t>(50.0f + (l - 1.0f) * 100.0f * (1.0f + l / 10.0f));
}

int xp_for_kill(int enemy_max_hp, float xp_mult) noexcept {
    return static_cast<int>(std::ceil(static_cast<float>(enemy_max_hp) / 10.0f) * xp_mult);
}

int capped_xp(int xp, int level) noexcept {
    const float max = static_cast<float>(xp_to_next_level(level)) * 0.1f;
    return static_cast<int>(std::min(static_cast<float>(xp), max));
}

float attack_period_ms(int dexterity, float rate_of_fire) noexcept {
    const float dex = static_cast<float>(std::clamp(dexterity, 0, 200));
    const float per_ms = 0.0015f + dex / 75.0f * 0.0065f;
    return 1.0f / per_ms / (rate_of_fire > 0.0f ? rate_of_fire : 1.0f);
}

int damage_with_attack(int roll, int attack, bool weak, bool damaging) noexcept {
    double mult = weak ? 0.5 : 0.5 + attack / 50.0;
    if (damaging) mult *= 1.5;
    return static_cast<int>(std::lround(roll * mult));
}

int after_defense(int damage, int defense, bool armor_piercing, bool armored) noexcept {
    if (damage <= 0) return 0;
    if (armor_piercing) return damage;
    const int def = std::max(0, defense) * (armored ? 2 : 1);
    const int least = static_cast<int>(damage * 0.15);
    return std::max(damage - def, least);
}

float hp_regen_per_second(int vitality) noexcept { return 1.0f + 0.12f * static_cast<float>(vitality); }
float mp_regen_per_second(int wisdom) noexcept { return 0.5f + 0.06f * static_cast<float>(wisdom); }

bool FireRateBucket::try_attack(double now_ms, float period_ms) noexcept {
    constexpr float kCapacity = 3.0f;
    constexpr float kSlack = 1.3f;
    if (last_ms_ >= 0.0 && period_ms > 0.0f) {
        const float refill = static_cast<float>(now_ms - last_ms_) / period_ms * kSlack;
        shots_ = std::min(kCapacity, shots_ + refill);
    }
    last_ms_ = now_ms;
    if (shots_ - 1.0f < -0.5f) return false;
    shots_ -= 1.0f;
    return true;
}

}  // namespace waw::sim::rules
