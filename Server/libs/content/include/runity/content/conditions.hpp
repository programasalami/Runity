#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace runity::content {

/// The original's condition effects (Common/Enumerables.cs ConditionEffectIndex). An entity's replicated `conditions` field is a
/// bit set: bit N = effect N is on. Only the first 64 exist on the wire (EntityFull.conditions is a u64).
enum class Condition : std::uint8_t {
    Nothing = 0, Dead, Quiet, Weak, Slowed, Sick, Dazed, Stunned, Blind, Hallucinating, Drunk, Confused, StunImmune, Invisible, Paralyzed,
    Speedy, Bleeding, ArmorBrokenImmune, Healing, Damaging, Berserk, Paused, Stasis, StasisImmune, Invincible, Invulnerable, Armored,
    ArmorBroken, Hexed, NinjaSpeedy, Unstable, Darkness, SlowedImmune, DazedImmune, ParalyzedImmune, Petrify, PetrifiedImmune,
    PetEffectIcon, Curse, CurseImmune, HpBoost, MpBoost, AttBoost, DefBoost, SpdBoost, VitBoost, WisBoost, DexBoost, Silenced, Exposed,
    Energized, HpDebuff, MpDebuff, AttDebuff, DefDebuff, SpdDebuff, VitDebuff, WisDebuff, DexDebuff, Inspired, Count
};

[[nodiscard]] constexpr std::uint64_t condition_bit(Condition c) noexcept { return std::uint64_t{1} << static_cast<unsigned>(c); }

/// "Invulnerable" -> Condition::Invulnerable (the names the behaviours and XML use).
[[nodiscard]] std::optional<Condition> condition_from_string(std::string_view name) noexcept;

}  // namespace runity::content
