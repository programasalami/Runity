#pragma once

#include <array>
#include <cstdint>
#include <random>

#include "waw/content/content_db.hpp"

namespace waw::sim::rules {

// Pure game formulas: the original Alloy's where it has one (modern GameServer, else LEGACY GameServerOld), otherwise the decision
// recorded in Documentation/Migration/Combat.md. No state, no I/O: every system calls these.

struct PlayerStats {
    int max_hp = 0;
    int max_mp = 0;
    int attack = 0;
    int defense = 0;
    int speed = 0;
    int dexterity = 0;
    int vitality = 0;  // <HpRegen> in Players.xml
    int wisdom = 0;    // <MpRegen>
    friend bool operator==(const PlayerStats&, const PlayerStats&) = default;
};

using BaseStats = std::array<int, content::kStatCount>;

/// A new character's stats: every class start value (the original DbClient.CreateCharacterAsync).
[[nodiscard]] BaseStats starting_stats(const content::PlayerClassDesc& cls) noexcept;
/// One level-up: every stat grows by a random amount in the class's <LevelIncrease min max> (classic RotMG, data in Players.xml;
/// the original parses it but never levels up), never above the class maximum.
void level_up(BaseStats& stats, const content::PlayerClassDesc& cls, std::mt19937_64& rng);
/// Base + what the worn items (slots 0-3) give (the original's ActivateOnEquip boosts).
[[nodiscard]] PlayerStats with_gear(const BaseStats& base, const std::vector<int>& items, const content::ContentDb& content) noexcept;

// Inventory layout (the original: 4 class slots + 8 inventory + 8 backpack = 20, EntityInventory).
inline constexpr int kGearSlots = 4;
inline constexpr int kBackpackSlot = 12;
inline constexpr int kPlayerSlots = 20;
/// Can an item of `item_slot_type` sit in player slot `slot` of a class with these gear slot types? (EntityInventory.IsEquippable)
[[nodiscard]] bool slot_fits(int slot, const std::vector<int>& class_slot_types, int item_slot_type) noexcept;

/// Level cap: 20 (Players.xml / RotMG design; the original has no level-up, LEGACY used 50 "for testing").
inline constexpr int kMaxLevel = 20;
/// (int)(50 + (L - 1) * 100 * (1 + L / 10f)) (PlayerExtensions.cs / LEGACY Player.GetNextLevelXPGoal). XP is progress in the level.
[[nodiscard]] std::int64_t xp_to_next_level(int level) noexcept;
/// (int)(ceil(maxHp / 10f) * xpMult) (LEGACY CharacterEntity.HandleXpGain), given to every player near the monster.
[[nodiscard]] int xp_for_kill(int enemy_max_hp, float xp_mult) noexcept;
/// One kill never gives more than 10 % of the player's next-level goal (LEGACY Player.CalculateXPGain; 50 % for a quest).
[[nodiscard]] int capped_xp(int xp, int level) noexcept;
/// Fame: 1 per 500 XP gained (LEGACY Player.XPPerFame).
inline constexpr std::int64_t kXpPerFame = 500;
/// A kill's XP reaches players within this squared distance of the monster (LEGACY: SIGHT_RADIUS_SQR * 2).
[[nodiscard]] constexpr float xp_share_distance_sq(float sight_radius) noexcept { return sight_radius * sight_radius * 2.0f; }

/// Milliseconds between attacks: 1 / (0.0015 + clamp(dex, 0, 200) / 75 * 0.0065) / rateOfFire (the original client Player.cs).
[[nodiscard]] float attack_period_ms(int dexterity, float rate_of_fire) noexcept;
/// round(roll * multiplier), multiplier = 0.5 + attack / 50 (LEGACY 0.5 + att / 75 * 1.5); Weak = 0.5; Damaging x1.5.
[[nodiscard]] int damage_with_attack(int roll, int attack, bool weak = false, bool damaging = false) noexcept;
/// max(damage - defense, (int)(damage * 0.15)) (LEGACY EntityUtils); Armored doubles defense; armor piercing / ArmorBroken ignore it.
[[nodiscard]] int after_defense(int damage, int defense, bool armor_piercing, bool armored = false) noexcept;

/// Regeneration per second: HP 1 + 0.12 * vitality, MP 0.5 + 0.06 * wisdom (kept from the port; the original has none).
[[nodiscard]] float hp_regen_per_second(int vitality) noexcept;
[[nodiscard]] float mp_regen_per_second(int wisdom) noexcept;

/// The server's fire-rate bucket: three attacks of burst, refilled at the attack rate with 30 % slack.
class FireRateBucket {
public:
    /// True if an attack at `now_ms` is allowed (and spends it).
    bool try_attack(double now_ms, float period_ms) noexcept;

private:
    double last_ms_ = -1.0;
    float shots_ = 3.0f;
};

}  // namespace waw::sim::rules
