/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MAGETRIGGERS_H
#define PLAYERBOTS_MAGETRIGGERS_H

#include "CureTriggers.h"
#include "GenericTriggers.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Trigger.h"
#include <unordered_set>

// Buff and Out of Combat Triggers

class ArcaneIntellectOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    ArcaneIntellectOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "arcane intellect", 8 * IN_MILLISECONDS) {}
};

class ArcaneIntellectTrigger : public BuffTrigger
{
public:
    ArcaneIntellectTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "arcane intellect", 8 * IN_MILLISECONDS) {}
    bool IsActive() override;
};

class MageArmorTrigger : public BuffTrigger
{
public:
    MageArmorTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "mage armor", 10 * IN_MILLISECONDS) {}
    bool IsActive() override;
};

class MoltenArmorTrigger : public BuffTrigger
{
public:
    MoltenArmorTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "molten armor", 10 * IN_MILLISECONDS) {}
    bool IsActive() override;
};

class NoFocusMagicTrigger : public Trigger
{
public:
    NoFocusMagicTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no focus magic") {}
    bool IsActive() override;
};

class IceBarrierTrigger : public BuffTrigger
{
public:
    IceBarrierTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "ice barrier") {}
};

class NoManaGemTrigger : public Trigger
{
public:
    NoManaGemTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no mana gem") {}
    bool IsActive() override;
};

// Instances, out of combat: one bot mage of the group sets up a Refreshment Table before a boss (ready check in
// progress or a boss near) when none is up. The ritual needs two more group members to click its portal.
class RitualOfRefreshmentTrigger : public Trigger
{
public:
    RitualOfRefreshmentTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ritual of refreshment", 5) {}
    bool IsActive() override;
};

class FireWardTrigger : public DeflectSpellTrigger
{
public:
    FireWardTrigger(PlayerbotAI* botAI) : DeflectSpellTrigger(botAI, "fire ward") {}
};

class FrostWardTrigger : public DeflectSpellTrigger
{
public:
    FrostWardTrigger(PlayerbotAI* botAI) : DeflectSpellTrigger(botAI, "frost ward") {}
};

// Proc and Boost Triggers

class HotStreakTrigger : public HasAuraTrigger
{
public:
    HotStreakTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "hot streak") {}
};

class FirestarterTrigger : public HasAuraTrigger
{
public:
    FirestarterTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "firestarter") {}
};

class MissileBarrageTrigger : public HasAuraTrigger
{
public:
    MissileBarrageTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "missile barrage") {}
};

class ArcaneBlastStackTrigger : public HasAuraStackTrigger
{
public:
    ArcaneBlastStackTrigger(PlayerbotAI* botAI) : HasAuraStackTrigger(botAI, "arcane blast", 4, 1) {}
};

class ArcaneBlast4StacksAndMissileBarrageTrigger : public TwoTriggers
{
public:
    ArcaneBlast4StacksAndMissileBarrageTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "arcane blast stack", "missile barrage") {}
};

// [b31_deep] wowsims arcane / frost APL: Arcane Blast while mana > 25 %, Evocation at <= 25 %. One threshold for
// the Arcane conserve phase and for Evocation.
constexpr uint8 MAGE_CONSERVE_MANA_PCT = 25;

class ManaAtOrBelow25Trigger : public Trigger
{
public:
    ManaAtOrBelow25Trigger(PlayerbotAI* botAI) : Trigger(botAI, "mana at or below 25") {}
    bool IsActive() override
    {
        return AI_VALUE2(bool, "has mana", "self target") &&
               AI_VALUE2(uint8, "mana", "self target") <= MAGE_CONSERVE_MANA_PCT;
    }
};

// [rot250] Evocation at <= 25 % mana. Arcane: in the wowsims arcane APL's place - after "Arcane Blast while stacks < 4"
// and "Missile Barrage -> Arcane Missiles" - so it is channelled only at 4 Arcane Blast stacks with no Missile Barrage
// proc up (or once Arcane Blast can no longer be paid). The 8 s channel outlasts the 6 s stack aura (36032), and a
// proc kept through it and the 4 Arcane Blasts after it (15 s aura, 44401) runs out: b218 rfc-h 5D Rtmagac cast 60
// Arcane Blasts to 10 Arcane Missiles on the 4 bosses (1 : 6), the sim 82 to 20 (1 : 4). Fire / Frost: <= 25 % only
// (their APLs).
class EvocationTrigger : public Trigger
{
public:
    EvocationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "evocation") {}
    bool IsActive() override;
};

// Conserve phase: at 4 Arcane Blast stacks and <= 25 % mana, Arcane Missiles drops the stacks.
// [b31_deep] was "medium mana" (40 %): the burn phase ended 15 points of mana before the wowsims APL.
class ArcaneBlast4StacksAndConserveManaTrigger : public TwoTriggers
{
public:
    ArcaneBlast4StacksAndConserveManaTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "arcane blast stack", "mana at or below 25")
    {
    }
};

class CombustionTrigger : public BoostTrigger
{
public:
    CombustionTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "combustion") {}
};

// [b31_deep] Cold Snap for a second Icy Veins: as soon as Icy Veins has run out and is on cooldown, inside the same
// burst gating as Icy Veins (BoostTrigger). The Deep Freeze cooldown condition is gone: in a group Deep Freeze is
// used only on stun-immune mobs, so Cold Snap almost never fired. Aura / cooldown checks only.
class ColdSnapTrigger : public BoostTrigger
{
public:
    ColdSnapTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "cold snap") {}
    bool IsActive() override;
};

class MirrorImageTrigger : public BoostTrigger
{
public:
    MirrorImageTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "mirror image") {}
};

class IcyVeinsTrigger : public BoostTrigger
{
public:
    IcyVeinsTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "icy veins") {}
};

class ArcanePowerTrigger : public BoostTrigger
{
public:
    ArcanePowerTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "arcane power") {}
};
class PresenceOfMindTrigger : public BoostTrigger
{
public:
    PresenceOfMindTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "presence of mind") {}
    // [b31_deep] Spell.dbc: Presence of Mind has excludeCasterAuraSpell = Arcane Power (12042), so under Arcane
    // Power the cast fails on CASTER_AURASTATE every tick.
    bool IsActive() override { return !bot->HasAura(12042) && BoostTrigger::IsActive(); }
};

// CC, Interrupt, and Dispel Triggers

class PolymorphTrigger : public HasCcTargetTrigger
{
public:
    PolymorphTrigger(PlayerbotAI* botAI) : HasCcTargetTrigger(botAI, "polymorph") {}
};

class RemoveCurseTrigger : public NeedCureTrigger
{
public:
    RemoveCurseTrigger(PlayerbotAI* botAI) : NeedCureTrigger(botAI, "remove curse", DISPEL_CURSE) {}
};

class PartyMemberRemoveCurseTrigger : public PartyMemberNeedCureTrigger
{
public:
    PartyMemberRemoveCurseTrigger(PlayerbotAI* botAI)
        : PartyMemberNeedCureTrigger(botAI, "remove curse", DISPEL_CURSE) {}
};

class SpellstealTrigger : public TargetAuraDispelTrigger
{
public:
    SpellstealTrigger(PlayerbotAI* botAI) : TargetAuraDispelTrigger(botAI, "spellsteal", DISPEL_MAGIC) {}
};

class CounterspellEnemyHealerTrigger : public InterruptEnemyHealerTrigger
{
public:
    CounterspellEnemyHealerTrigger(PlayerbotAI* botAI) : InterruptEnemyHealerTrigger(botAI, "counterspell") {}
};

class CounterspellInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    CounterspellInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "counterspell") {}
};

// Damage and Debuff Triggers

// [b31_deep] Living Bomb explodes only on expiry or dispel (spell_mage_living_bomb::AfterRemove), not when the
// target dies, and the explosion is ~40-50 % of its damage: only on a target expected to live its 12 s.
constexpr float LIVING_BOMB_MIN_TARGET_LIFETIME = 12.0f;

class LivingBombTrigger : public DebuffTrigger
{
public:
    LivingBombTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "living bomb", 1, true, LIVING_BOMB_MIN_TARGET_LIFETIME) {}
};

class LivingBombOnAttackersTrigger : public DebuffOnAttackerTrigger
{
public:
    // [guide_b31_dead] multidot only with 2+ attackers, on an attacker expected to live >= 12 s (the bomb's
    // duration); the cheap cached count runs before the attacker scan.
    LivingBombOnAttackersTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "living bomb", true, LIVING_BOMB_MIN_TARGET_LIFETIME) {}  // [b31_deep]
    bool IsActive() override
    {
        return AI_VALUE(uint8, "attacker count") >= 2 && DebuffTrigger::IsActive();
    }
};

class FireballTrigger : public DebuffTrigger
{
public:
    FireballTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "fireball", 1, true) {}
};

// [ranged_audit_b30] Build 30: fire mages at 80 opened every trash mob with Scorch (81 Scorch to 82 Fireball,
// 1.9k per 1.5 s cast against 5.2k per 2.5 s Fireball) because the debuff only asked for a 0.5 s lifetime. The
// +5 % crit debuff repays the Scorch GCD only on a target the casters keep hitting: ~12 s by the group's damage.
constexpr float IMPROVED_SCORCH_MIN_TARGET_LIFETIME = 12.0f;

class ImprovedScorchTrigger : public DebuffTrigger
{
public:
    ImprovedScorchTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "improved scorch", 1, true, IMPROVED_SCORCH_MIN_TARGET_LIFETIME) {}
    bool IsActive() override;
};

// [rot251] wowsims fire APL, the fight's end: Fire Blast once the fight has no more than a Scorch cast left, Scorch
// once it has <= 4 s left. The fight's remaining seconds as the APL's remainingTime: the longest ExpectedLifetime of
// the target and the mage's attackers.
constexpr float FIRE_FIGHT_END_SCORCH_SEC = 4.0f;

class FireBlastFightEndTrigger : public Trigger
{
public:
    FireBlastFightEndTrigger(PlayerbotAI* botAI) : Trigger(botAI, "fire blast fight end") {}
    std::string const GetTargetName() override { return "current target"; }
    bool IsActive() override;
};

class ScorchFightEndTrigger : public Trigger
{
public:
    ScorchFightEndTrigger(PlayerbotAI* botAI) : Trigger(botAI, "scorch fight end") {}
    std::string const GetTargetName() override { return "current target"; }
    bool IsActive() override;
};

class PyroblastTrigger : public DebuffTrigger
{
public:
    PyroblastTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "pyroblast", 1, true) {}
};

class FrostfireBoltTrigger : public DebuffTrigger
{
public:
    FrostfireBoltTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "frostfire bolt", 1, true) {}
};

class FingersOfFrostTrigger : public HasAuraTrigger
{
public:
    FingersOfFrostTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "fingers of frost") {}
};

class BrainFreezeTrigger : public HasAuraTrigger
{
public:
    BrainFreezeTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "fireball!") {}
};

class FrostNovaOnTargetTrigger : public DebuffTrigger
{
public:
    FrostNovaOnTargetTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "frost nova", 1, false) {}
    bool IsActive() override;
};

// A mob hits the group's healer within Frost Nova reach of the mage: rooting it lets the healer step away while the
// tank's taunts are on cooldown (wave 8 sample: Hand of Reckoning unusable in 87% of the peel ticks).
class HealerAttackerInFrostNovaRangeTrigger : public Trigger
{
public:
    HealerAttackerInFrostNovaRangeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "healer attacker in frost nova range")
    {
    }

    bool IsActive() override;
};

class FrostbiteOnTargetTrigger : public DebuffTrigger
{
public:
    FrostbiteOnTargetTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "frostbite", 1, false) {}
    bool IsActive() override;
};

class FlamestrikeNearbyTrigger : public Trigger
{
public:
    FlamestrikeNearbyTrigger(PlayerbotAI* botAI, float radius = 30.0f)
        : Trigger(botAI, "flamestrike nearby"), radius(radius)
    {
    }
    bool IsActive() override;

protected:
    float radius;
    static const std::unordered_set<uint32> FLAMESTRIKE_SPELL_IDS;
};

class FlamestrikeBlizzardTrigger : public TwoTriggers
{
public:
    FlamestrikeBlizzardTrigger(PlayerbotAI* botAI) : TwoTriggers(botAI, "flamestrike nearby", "medium aoe") {}
};

class BlizzardChannelCheckTrigger : public Trigger
{
public:
    BlizzardChannelCheckTrigger(PlayerbotAI* botAI, uint32 minEnemies = 2)
        : Trigger(botAI, "blizzard channel check"), minEnemies(minEnemies) {}

    bool IsActive() override;

protected:
    uint32 minEnemies;
    static const std::unordered_set<uint32> BLIZZARD_SPELL_IDS;
};

class BlastWaveOffCdTrigger : public SpellNoCooldownTrigger
{
public:
    BlastWaveOffCdTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "blast wave") {}
};

class BlastWaveOffCdTriggerAndMediumAoeTrigger : public TwoTriggers
{
public:
    BlastWaveOffCdTriggerAndMediumAoeTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "blast wave off cd", "medium aoe") {}
};

class NoFirestarterStrategyTrigger : public Trigger
{
public:
    NoFirestarterStrategyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no firestarter strategy") {}

    bool IsActive() override
    {
        return !botAI->HasStrategy("firestarter", BOT_STATE_COMBAT);
    }
};

class EnemyIsCloseAndNoFirestarterStrategyTrigger : public TwoTriggers
{
public:
    EnemyIsCloseAndNoFirestarterStrategyTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy is close", "no firestarter strategy") {}
};

class EnemyTooCloseForSpellAndNoFirestarterStrategyTrigger : public TwoTriggers
{
public:
    EnemyTooCloseForSpellAndNoFirestarterStrategyTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy too close for spell", "no firestarter strategy") {}
};

// Point-blank knockback/disorient pulls mobs off the tank, so in a group it is self-defence only
class SoloOrBeingAttackedTrigger : public Trigger
{
public:
    SoloOrBeingAttackedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "solo or being attacked") {}

    bool IsActive() override { return !bot->GetGroup() || AI_VALUE(uint8, "my attacker count") > 0; }
};

class EnemyIsCloseAndSoloOrBeingAttackedTrigger : public TwoTriggers
{
public:
    EnemyIsCloseAndSoloOrBeingAttackedTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy is close", "solo or being attacked") {}
};

class EnemyTooCloseForSpellAndSoloOrBeingAttackedTrigger : public TwoTriggers
{
public:
    EnemyTooCloseForSpellAndSoloOrBeingAttackedTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy too close for spell", "solo or being attacked") {}
};

// [b31_deep] Frost Nova / Blink back in a group only in self-defence: "enemy is close" and "enemy too close for
// spell" fire on a mob the tank holds, so each cost a GCD (Arcane 60s: Blink 2.1/min on bosses, Nova 6 % of all
// actions) and the nova rooted mobs the tank was gathering.
class EnemyIsCloseNoFirestarterAndSoloOrBeingAttackedTrigger : public TwoTriggers
{
public:
    EnemyIsCloseNoFirestarterAndSoloOrBeingAttackedTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy is close and no firestarter strategy", "solo or being attacked") {}
};

class EnemyTooCloseNoFirestarterAndSoloOrBeingAttackedTrigger : public TwoTriggers
{
public:
    EnemyTooCloseNoFirestarterAndSoloOrBeingAttackedTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy too close for spell and no firestarter strategy", "solo or being attacked") {}
};

// [b31_deep] "has pet" re-ran "toggle pet spell" every 5 s for the permanent Water Elemental (Eternal Water) and it
// returned false 31 548 times in two days: a 30 s check interval is enough for a pet that rarely changes.
class WaterElementalAutocastCheckTrigger : public Trigger
{
public:
    WaterElementalAutocastCheckTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "water elemental autocast check", 30 * IN_MILLISECONDS) {}
    bool IsActive() override
    {
        return AI_VALUE(Unit*, "pet target") && !AI_VALUE2(bool, "mounted", "self target");
    }
};

#endif
