/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WARLOCKTRIGGERS_H
#define PLAYERBOTS_WARLOCKTRIGGERS_H

#include "CureTriggers.h"
#include "EstimatedLifetimeValue.h"
#include "GenericTriggers.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidUtilityAssignment.h"
#include "RtiTriggers.h"
#include "Trigger.h"
#include "WarlockActions.h"
#include <set>

class PlayerbotAI;

namespace ai::warlock
{
    // Curse the raid utility coordinator gives this warlock; NONE when it keeps a player-chosen curse strategy.
    ai::raidutil::AssignedCurse GetCoordinatedCurse(PlayerbotAI* botAI);
    // False when the coordinator gave this warlock a different curse (one curse per warlock per target).
    bool IsCurseAllowed(PlayerbotAI* botAI, ai::raidutil::AssignedCurse curse);
}

// Buff and Out of Combat Triggers

class DemonArmorTrigger : public BuffTrigger
{
public:
    DemonArmorTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "demon armor") {}

    bool IsActive() override;
};

class SoulLinkTrigger : public BuffTrigger
{
public:
    SoulLinkTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "soul link") {}
    bool IsActive() override;
};

class UnendingBreathTrigger : public BuffTrigger
{
public:
    UnendingBreathTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "unending breath", 10 * IN_MILLISECONDS) {}
    bool IsActive() override;
};

class UnendingBreathOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    UnendingBreathOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "unending breath on party", 4 * IN_MILLISECONDS) {}
    bool IsActive() override;
};

class OutOfSoulShardsTrigger : public Trigger
{
public:
    OutOfSoulShardsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no soul shard", 2) {}
    bool IsActive() override;
};

class TooManySoulShardsTrigger : public Trigger
{
public:
    TooManySoulShardsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "too many soul shards") {}
    bool IsActive() override;
};

class FirestoneTrigger : public BuffTrigger
{
public:
    FirestoneTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "firestone") {}
    bool IsActive() override;
};

class SpellstoneTrigger : public BuffTrigger
{
public:
    SpellstoneTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "spellstone") {}
    bool IsActive() override;
};

class OutOfSoulstoneTrigger : public Trigger
{
public:
    OutOfSoulstoneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "no soulstone") {}
    bool IsActive() override;
};

class SoulstoneTrigger : public Trigger
{
public:
    SoulstoneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "soulstone") {}

    bool IsActive() override
    {
        static const std::vector<uint32> soulstoneSpellIds = {20707, 20762, 20763, 20764, 20765, 27239, 47883};

        if (AI_VALUE2(uint32, "item count", "soulstone") == 0)
            return false;

        for (uint32 spellId : soulstoneSpellIds)
        {
            if (!bot->HasSpellCooldown(spellId))
                return true;  // Ready to use
        }

        return false;  // All are on cooldown
    }
};

// Raids and heroic dungeons, out of combat: one bot warlock of the group opens a Soulwell before a boss (ready check
// in progress or a boss near) when none is up. The leader layer makes the others click the Soul Portal and the well.
class RitualOfSoulsTrigger : public Trigger
{
public:
    RitualOfSoulsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ritual of souls", 5) {}
    bool IsActive() override;
};

class WarlockConjuredItemTrigger : public ItemCountTrigger
{
public:
    WarlockConjuredItemTrigger(PlayerbotAI* botAI, std::string const item) : ItemCountTrigger(botAI, item, 1) {}
    bool IsActive() override;
};

class HasSpellstoneTrigger : public WarlockConjuredItemTrigger
{
public:
    HasSpellstoneTrigger(PlayerbotAI* botAI) : WarlockConjuredItemTrigger(botAI, "spellstone") {}
};

class HasFirestoneTrigger : public WarlockConjuredItemTrigger
{
public:
    HasFirestoneTrigger(PlayerbotAI* botAI) : WarlockConjuredItemTrigger(botAI, "firestone") {}
};

class HasHealthstoneTrigger : public WarlockConjuredItemTrigger
{
public:
    HasHealthstoneTrigger(PlayerbotAI* botAI) : WarlockConjuredItemTrigger(botAI, "healthstone") {}
};

class WrongPetTrigger : public Trigger
{
public:
    WrongPetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "wrong pet") {}
    bool IsActive() override;
};

// CC and Pet Triggers

class BanishTrigger : public RtiCcTrigger
{
public:
    BanishTrigger(PlayerbotAI* botAI) : RtiCcTrigger(botAI, "banish") {}
};

class FearTrigger : public RtiCcTrigger
{
public:
    FearTrigger(PlayerbotAI* botAI) : RtiCcTrigger(botAI, "fear") {}
};

class SpellLockInterruptSpellTrigger : public InterruptSpellTrigger
{
public:
    SpellLockInterruptSpellTrigger(PlayerbotAI* botAI) : InterruptSpellTrigger(botAI, "spell lock") {}
};

class DevourMagicPurgeTrigger : public TargetAuraDispelTrigger
{
public:
    DevourMagicPurgeTrigger(PlayerbotAI* botAI) : TargetAuraDispelTrigger(botAI, "devour magic", DISPEL_MAGIC) {}
};

class DevourMagicCleanseTrigger : public PartyMemberNeedCureTrigger
{
public:
    DevourMagicCleanseTrigger(PlayerbotAI* botAI) : PartyMemberNeedCureTrigger(botAI, "devour magic", DISPEL_MAGIC) {}
};

// DoT/Curse Triggers

class CorruptionTrigger : public DebuffTrigger
{
public:
    CorruptionTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "corruption", 1, true, CORRUPTION_MIN_TARGET_LIFETIME, CORRUPTION_REFRESH_MS) {}
    bool IsActive() override
    {
        return DebuffTrigger::IsActive() && !botAI->HasAura("seed of corruption", GetTarget(), false, true);
    }
};

// Without Affliction talents Corruption only beats the filler when the target outlives a full application
class CorruptionLongFightTrigger : public DebuffTrigger
{
public:
    CorruptionLongFightTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "corruption", 1, true, CORRUPTION_FULL_DURATION_SECONDS, CORRUPTION_REFRESH_MS) {}
    std::string const getName() override { return "corruption long fight"; }
    bool IsActive() override
    {
        return DebuffTrigger::IsActive() && !botAI->HasAura("seed of corruption", GetTarget(), false, true);
    }
};

class CorruptionOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    CorruptionOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "corruption", true, CORRUPTION_MIN_TARGET_LIFETIME) {}
    bool IsActive() override
    {
        return DebuffTrigger::IsActive() && !botAI->HasAura("seed of corruption", GetTarget(), false, true);
    }
};

class ImmolateTrigger : public DebuffTrigger
{
public:
    ImmolateTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "immolate", 1, true, IMMOLATE_MIN_TARGET_LIFETIME, IMMOLATE_REFRESH_MS) {}
};

class ImmolateOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    ImmolateOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "immolate", true, IMMOLATE_MIN_TARGET_LIFETIME) {}
};

// [b31_deep] Demonology main-target Immolate: wowsims demo APL casts it only while the fight lasts >= 12 s.
class DemoImmolateTrigger : public DebuffTrigger
{
public:
    DemoImmolateTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "immolate", 1, true, DEMO_IMMOLATE_MIN_TARGET_LIFETIME, IMMOLATE_REFRESH_MS) {}
    std::string const getName() override { return "demo immolate"; }
};

// [b31_deep] Demonology multi-dot: wowsims demo APL keeps Corruption on the main target only (maxDots 1). Other
// attackers get Corruption / Immolate only in a pack (2+ attackers) and when they live
// DEMO_MULTIDOT_MIN_TARGET_LIFETIME; the strategy ranks these below the Decimation / Molten Core procs.
class DemoCorruptionOnAttackerTrigger : public CorruptionOnAttackerTrigger
{
public:
    DemoCorruptionOnAttackerTrigger(PlayerbotAI* botAI) : CorruptionOnAttackerTrigger(botAI)
    {
        needLifeTime = DEMO_MULTIDOT_MIN_TARGET_LIFETIME;
    }
    std::string const getName() override { return "demo corruption on attacker"; }
    bool IsActive() override
    {
        return AI_VALUE(uint8, "attacker count") >= 2 && CorruptionOnAttackerTrigger::IsActive();
    }
};

class DemoImmolateOnAttackerTrigger : public ImmolateOnAttackerTrigger
{
public:
    DemoImmolateOnAttackerTrigger(PlayerbotAI* botAI) : ImmolateOnAttackerTrigger(botAI)
    {
        needLifeTime = DEMO_MULTIDOT_MIN_TARGET_LIFETIME;
    }
    std::string const getName() override { return "demo immolate on attacker"; }
    bool IsActive() override
    {
        return AI_VALUE(uint8, "attacker count") >= 2 && ImmolateOnAttackerTrigger::IsActive();
    }
};

class UnstableAfflictionTrigger : public DebuffTrigger
{
public:
    UnstableAfflictionTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "unstable affliction", 1, true, UNSTABLE_AFFLICTION_MIN_TARGET_LIFETIME,
                        UNSTABLE_AFFLICTION_REFRESH_MS) {}
};

class UnstableAfflictionOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    UnstableAfflictionOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "unstable affliction", true, UNSTABLE_AFFLICTION_MIN_TARGET_LIFETIME) {}
};

// Execute phase: Drain Soul deals four times its damage while the target is at or below 25 % health (3.3.5).
// Its ticks also roll Corruption through Everlasting Affliction, so it replaces Shadow Bolt as the filler.
class DrainSoulExecuteTrigger : public Trigger
{
public:
    DrainSoulExecuteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "drain soul execute") {}
    std::string const GetTargetName() override { return "current target"; }
    bool IsActive() override
    {
        Unit* target = GetTarget();
        if (!target || !target->IsAlive() || target->HealthAbovePct(DRAIN_SOUL_EXECUTE_HEALTH_PCT))
            return false;

        // [ranged_audit_b30] Its first tick comes 3 s into the channel: build 30 trash at <= 25 % died before it
        // (804 damage per Drain Soul against 3.6k per Shadow Bolt, ~10 % of the affliction warlock's time). Only a
        // target the group still needs DRAIN_SOUL_EXECUTE_MIN_TARGET_LIFETIME to kill gets the channel.
        // [b31_deep] Same estimator as CastTimeMultiplier (ExpectedLifetime: the measured health drop as well as
        // the dps model). The model alone let 105 channels start that the multiplier had already cut to x0.1
        // ("drain soul (1.55)"): no tick landed and the channel blocked Haunt / Shadow Bolt.
        return ExpectedLifetime(botAI, target) >= DRAIN_SOUL_EXECUTE_MIN_TARGET_LIFETIME;
    }
};

// Haunt is used on cooldown: its debuff outlasts the cooldown and it refreshes Corruption
class HauntTrigger : public SpellNoCooldownTrigger
{
public:
    HauntTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "haunt") {}
    bool IsActive() override { return GetTarget() && SpellNoCooldownTrigger::IsActive(); }
};

// [guide_b31_dead] Destruction: Conflagrate consumes the bot's own Immolate or Shadowflame DoT, so it needs one
// on the target; off cooldown otherwise. Previously unregistered, so the 19.5 strategy node never fired.
class ConflagrateTrigger : public SpellNoCooldownTrigger
{
public:
    ConflagrateTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "conflagrate") {}
    bool IsActive() override
    {
        Unit* target = GetTarget();
        if (!target || !SpellNoCooldownTrigger::IsActive())
            return false;
        return botAI->HasAura("immolate", target, false, true) || botAI->HasAura("shadowflame", target, false, true);
    }
};

// [guide_b31_dead] Destruction: Chaos Bolt on cooldown. Previously unregistered, so the 19.0 node never fired.
class ChaosBoltTrigger : public SpellNoCooldownTrigger
{
public:
    ChaosBoltTrigger(PlayerbotAI* botAI) : SpellNoCooldownTrigger(botAI, "chaos bolt") {}
    bool IsActive() override { return GetTarget() && SpellNoCooldownTrigger::IsActive(); }
};

class CurseOfAgonyTrigger : public DebuffTrigger
{
public:
    CurseOfAgonyTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "curse of agony", 1, true, CURSE_OF_AGONY_MIN_TARGET_LIFETIME) {}
    bool IsActive() override;
};

class CurseOfAgonyOnAttackerTrigger : public DebuffOnAttackerTrigger
{
public:
    CurseOfAgonyOnAttackerTrigger(PlayerbotAI* botAI)
        : DebuffOnAttackerTrigger(botAI, "curse of agony", true, CURSE_OF_AGONY_MIN_TARGET_LIFETIME) {}
    bool IsActive() override;
};

class CurseOfTheElementsTrigger : public DebuffTrigger
{
public:
    CurseOfTheElementsTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "curse of the elements", 1, true, CURSE_OF_THE_ELEMENTS_MIN_TARGET_LIFETIME) {}
    bool IsActive() override;
};

class CurseOfDoomTrigger : public DebuffTrigger
{
public:
    CurseOfDoomTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "curse of doom", 1, true, CURSE_OF_DOOM_MIN_TARGET_LIFETIME) {}
};

// [b31_deep] wowsims demo / destro APL: Curse of Doom when the fight lasts more than 60 s, else Curse of Agony.
// It takes the damage-curse (AGONY) slot of the raid utility coordinator, or the uncoordinated "curse of agony"
// strategy; Curse of the Elements / Weakness / Tongues owners keep theirs. Off cooldown (1 min), and the target lives
// a full application by both lifetime estimators (the only hit lands when the curse expires).
class DamageCurseOfDoomTrigger : public CurseOfDoomTrigger
{
public:
    DamageCurseOfDoomTrigger(PlayerbotAI* botAI) : CurseOfDoomTrigger(botAI) {}
    std::string const getName() override { return "damage curse of doom"; }
    bool IsActive() override;
};

// [b35_lock] wowsims destro: Inferno while the fight has less than INFERNO_MAX_TARGET_LIFETIME left (on a boss pull
// that ends within it, at once). Bosses only: its 2 s AoE stun breaks crowd control on trash. Cooldown ready
// (10 min), an Infernal Stone in the bags.
class InfernoBossTrigger : public Trigger
{
public:
    InfernoBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "inferno boss") {}
    std::string const GetTargetName() override { return "current target"; }
    bool IsActive() override;
};

class CurseOfExhaustionTrigger : public DebuffTrigger
{
public:
    CurseOfExhaustionTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "curse of exhaustion", 1, true, 0.5f) {}
    bool IsActive() override { return BuffTrigger::IsActive(); }
};

class CurseOfTonguesTrigger : public DebuffTrigger
{
public:
    CurseOfTonguesTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "curse of tongues", 1, true, 0.5f) {}
    bool IsActive() override;
};

class CurseOfWeaknessTrigger : public DebuffTrigger
{
public:
    // [b31_deep] Lifetime gate as for Curse of the Elements (was 0.5 s and unused).
    CurseOfWeaknessTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "curse of weakness", 1, true, CURSE_OF_THE_ELEMENTS_MIN_TARGET_LIFETIME) {}
    bool IsActive() override;
};

// Raid utility coordinator: the curse assigned to this warlock, whatever its spec curse strategy is.
class AssignedCurseOfTheElementsTrigger : public CurseOfTheElementsTrigger
{
public:
    AssignedCurseOfTheElementsTrigger(PlayerbotAI* botAI) : CurseOfTheElementsTrigger(botAI) {}
    std::string const getName() override { return "assigned curse of the elements"; }
    bool IsActive() override;
};

class AssignedCurseOfWeaknessTrigger : public CurseOfWeaknessTrigger
{
public:
    AssignedCurseOfWeaknessTrigger(PlayerbotAI* botAI) : CurseOfWeaknessTrigger(botAI) {}
    std::string const getName() override { return "assigned curse of weakness"; }
    bool IsActive() override;
};

class AssignedCurseOfTonguesTrigger : public CurseOfTonguesTrigger
{
public:
    AssignedCurseOfTonguesTrigger(PlayerbotAI* botAI) : CurseOfTonguesTrigger(botAI) {}
    std::string const getName() override { return "assigned curse of tongues"; }
    bool IsActive() override;
};

class AssignedCurseOfAgonyTrigger : public CurseOfAgonyTrigger
{
public:
    AssignedCurseOfAgonyTrigger(PlayerbotAI* botAI) : CurseOfAgonyTrigger(botAI) {}
    std::string const getName() override { return "assigned curse of agony"; }
    bool IsActive() override;
};

// Proc/Cooldown Triggers

class LifeTapTrigger : public Trigger
{
public:
    LifeTapTrigger(PlayerbotAI* botAI) : Trigger(botAI, "life tap") {}
    bool IsActive() override;
};

// [b31_deep] Refreshed shortly before it drops, not after (wowsims: affliction < 2 s; demo / destruction < 3 s,
// "life tap glyph buff early"); before, 0-1 GCD ran without the spirit-to-spell-power buff.
class LifeTapGlyphBuffTrigger : public BuffTrigger
{
public:
    LifeTapGlyphBuffTrigger(PlayerbotAI* botAI, uint32 refreshMs = LIFE_TAP_GLYPH_REFRESH_AFFLICTION_MS)
        : BuffTrigger(botAI, "life tap", 1, false, false, refreshMs) {}
    bool IsActive() override;
};

class LifeTapGlyphBuffEarlyTrigger : public LifeTapGlyphBuffTrigger
{
public:
    LifeTapGlyphBuffEarlyTrigger(PlayerbotAI* botAI) : LifeTapGlyphBuffTrigger(botAI, LIFE_TAP_GLYPH_REFRESH_MS) {}
    std::string const getName() override { return "life tap glyph buff early"; }
};

// K6 pre-pull: Life Tap for the Glyph of Life Tap spell power buff while the tank is about to pull.
class PullImminentLifeTapTrigger : public LifeTapGlyphBuffTrigger
{
public:
    PullImminentLifeTapTrigger(PlayerbotAI* botAI) : LifeTapGlyphBuffTrigger(botAI) {}
    std::string const getName() override { return "pull imminent life tap"; }
    bool IsActive() override;
};

class MetamorphosisTrigger : public BoostTrigger
{
public:
    MetamorphosisTrigger(PlayerbotAI* botAI) : BoostTrigger(botAI, "metamorphosis") {}
};

class DemonicEmpowermentTrigger : public BuffTrigger
{
public:
    DemonicEmpowermentTrigger(PlayerbotAI* botAI) : BuffTrigger(botAI, "demonic empowerment") {}
    bool IsActive() override;
};

class ImmolationAuraActiveTrigger : public HasAuraTrigger
{
public:
    ImmolationAuraActiveTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "immolation aura") {}
};

class ShadowTranceTrigger : public HasAuraTrigger
{
public:
    ShadowTranceTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "shadow trance") {}
};

class BacklashTrigger : public HasAuraTrigger
{
public:
    BacklashTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "backlash") {}
};

class DecimationTrigger : public HasAuraTrigger
{
public:
    DecimationTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "decimation") {}
    bool IsActive() override;
};

class MoltenCoreTrigger : public HasAuraTrigger
{
public:
    MoltenCoreTrigger(PlayerbotAI* botAI) : HasAuraTrigger(botAI, "molten core") {}
};

class MetamorphosisNotActiveTrigger : public HasNoAuraTrigger
{
public:
    MetamorphosisNotActiveTrigger(PlayerbotAI* botAI) : HasNoAuraTrigger(botAI, "metamorphosis") {}
};

class MetaMeleeEnemyTooCloseForSpellTrigger : public TwoTriggers
{
public:
    MetaMeleeEnemyTooCloseForSpellTrigger(PlayerbotAI* botAI)
        : TwoTriggers(botAI, "enemy too close for spell", "metamorphosis not active") {}
};

class RainOfFireChannelCheckTrigger : public Trigger
{
public:
    // [b31_deep] Counts the attackers inside the bot's own Rain of Fire (its DynamicObject), not in SightDistance
    // around the bot, which never fell below two and so never cancelled the channel.
    RainOfFireChannelCheckTrigger(PlayerbotAI* botAI, uint32 minEnemies = 2)
        : Trigger(botAI, "rain of fire channel check"), minEnemies(minEnemies)
    {
    }

    bool IsActive() override;

protected:
    uint32 minEnemies;
    static const std::set<uint32> RAIN_OF_FIRE_SPELL_IDS;
};

// Execute phase: a Drain Soul channel lasts 15 s, longer than Haunt's cooldown and Unstable Affliction's refresh.
// Clip it when Haunt is ready or Unstable Affliction is about to drop (Corruption is rolled by Drain Soul itself).
class DrainSoulClipTrigger : public Trigger
{
public:
    DrainSoulClipTrigger(PlayerbotAI* botAI) : Trigger(botAI, "drain soul clip") {}
    bool IsActive() override;
};

#endif
